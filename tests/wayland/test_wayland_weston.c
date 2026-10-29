/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _GNU_SOURCE

#include "boxrig/wayland.h"
#include "../../src/wayland/shm_private.h"
#if BOXRIG_TEST_WAYLAND_OPENGL
#include "boxrig/wayland/opengl_component.h"
#include "boxrig/render/opengl.h"
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

/* Native runs use the desktop's output scale and input capabilities. */
static bool native_display;
static bool capture_active;
static size_t captured_frames;
static bool fail_next_presentation;
static bool compare_reparent_frames;
static uint64_t reparent_frame_hash;
static size_t reparent_frame_count;

/* Save the submitted image, before desktop compositing, for a separate video
 * run. Captures never participate in the performance measurements. */
static void save_frame(const uint8_t *pixels, unsigned int width, unsigned int height,
    size_t stride, bool rgba_bottom_up)
{
    if (compare_reparent_frames) {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (unsigned int y = 0; y < height; y++) {
            const uint8_t *row = pixels + (rgba_bottom_up ? height - 1 - y : y) * stride;
            for (size_t x = 0; x < (size_t)width * 4; x++) {
                hash ^= row[x];
                hash *= UINT64_C(1099511628211);
            }
        }
        reparent_frame_hash = hash;
        reparent_frame_count++;
    }
    const char *directory = getenv("BOXRIG_TEST_RESIZE_CAPTURE");
    if (!capture_active || directory == NULL) return;
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/frame-%04zu.ppm", directory, captured_frames) > 0);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fprintf(file, "P6\n%u %u\n255\n", width, height) > 0);
    uint8_t *row = malloc((size_t)width * 3);
    CHECK(row != NULL);
    size_t nonbackground = 0;
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *source = pixels + (rgba_bottom_up ? height - 1 - y : y) * stride;
        for (unsigned int x = 0; x < width; x++) {
            if (rgba_bottom_up) memcpy(row + x * 3, source + x * 4, 3);
            else {
                uint32_t pixel;
                memcpy(&pixel, source + x * 4, sizeof(pixel));
                row[x * 3] = (uint8_t)(pixel >> 16);
                row[x * 3 + 1] = (uint8_t)(pixel >> 8);
                row[x * 3 + 2] = (uint8_t)pixel;
            }
            if (row[x * 3] > 100 || row[x * 3 + 1] > 100 || row[x * 3 + 2] > 100)
                nonbackground++;
        }
        CHECK(fwrite(row, 3, width, file) == width);
    }
    CHECK(fclose(file) == 0);
    free(row);
    CHECK(nonbackground > 20);
    printf("CAPTURE frame=%zu width=%u height=%u content_pixels=%zu\n",
        captured_frames++, width, height, nonbackground);
}

#if !BOXRIG_TEST_WAYLAND_OPENGL
static size_t copied_buffers;
boxrig_ui_status __real_boxrig_wayland_shm_pool_copy_cairo(boxrig_wayland_shm_pool *, cairo_surface_t *, uint64_t);
boxrig_ui_status __wrap_boxrig_wayland_shm_pool_copy_cairo(boxrig_wayland_shm_pool *pool,
    cairo_surface_t *surface, uint64_t generation)
{
    if (fail_next_presentation) {
        fail_next_presentation = false;
        return BOXRIG_UI_UNAVAILABLE;
    }
    copied_buffers += pool->buffer_count;
    boxrig_ui_status status = __real_boxrig_wayland_shm_pool_copy_cairo(pool, surface, generation);
    if (status == BOXRIG_UI_OK) save_frame(pool->mapping, (unsigned int)pool->width,
        (unsigned int)pool->height, (size_t)pool->stride, false);
    return status;
}
boxrig_ui_status __real_boxrig_wayland_shm_buffer_copy_cairo(boxrig_wayland_shm_pool *, size_t, cairo_surface_t *, uint64_t);
boxrig_ui_status __wrap_boxrig_wayland_shm_buffer_copy_cairo(boxrig_wayland_shm_pool *pool,
    size_t index, cairo_surface_t *surface, uint64_t generation)
{
    if (fail_next_presentation) {
        fail_next_presentation = false;
        return BOXRIG_UI_UNAVAILABLE;
    }
    copied_buffers++;
    boxrig_ui_status status = __real_boxrig_wayland_shm_buffer_copy_cairo(pool, index, surface, generation);
    if (status == BOXRIG_UI_OK) save_frame((uint8_t *)pool->mapping + index * pool->buffer_size,
        (unsigned int)pool->width, (unsigned int)pool->height, (size_t)pool->stride, false);
    return status;
}
#endif
#if BOXRIG_TEST_WAYLAND_OPENGL
static boxrig_ogl_renderer *observed_renderer;
static size_t renderer_creations;
static size_t egl_surface_creations;
static unsigned int blit_width, blit_height;
void __real_glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void __wrap_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
    GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    blit_width = (unsigned int)(dx1 - dx0);
    blit_height = (unsigned int)(dy1 - dy0);
    __real_glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
EGLBoolean __real_eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean __wrap_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    if (fail_next_presentation) {
        fail_next_presentation = false;
        return EGL_FALSE;
    }
    if (compare_reparent_frames ||
        (capture_active && getenv("BOXRIG_TEST_RESIZE_CAPTURE") != NULL)) {
        GLint source_framebuffer, source_buffer;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &source_framebuffer);
        glGetIntegerv(GL_READ_BUFFER, &source_buffer);
        CHECK(source_framebuffer != 0 && blit_width > 0 && blit_height > 0);
        size_t bytes = (size_t)blit_width * blit_height * 4;
        uint8_t *expected = calloc(1, bytes), *actual = calloc(1, bytes);
        CHECK(expected != NULL && actual != NULL);
        glReadPixels(0, 0, (GLsizei)blit_width, (GLsizei)blit_height, GL_RGBA, GL_UNSIGNED_BYTE, expected);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, (GLsizei)blit_width, (GLsizei)blit_height, GL_RGBA, GL_UNSIGNED_BYTE, actual);
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(memcmp(expected, actual, bytes) == 0);
        save_frame(actual, blit_width, blit_height, (size_t)blit_width * 4, true);
        free(actual);
        free(expected);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)source_framebuffer);
        glReadBuffer((GLenum)source_buffer);
    }
    return __real_eglSwapBuffers(display, surface);
}

boxrig_gl_status __real_boxrig_ogl_renderer_create(const boxrig_ogl_renderer_config *,
    boxrig_ogl_renderer **, boxrig_gl_diagnostic *);
boxrig_gl_status __wrap_boxrig_ogl_renderer_create(const boxrig_ogl_renderer_config *config,
    boxrig_ogl_renderer **renderer, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_gl_status status = __real_boxrig_ogl_renderer_create(config, renderer, diagnostic);
    if (status == BOXRIG_GL_OK) {
        observed_renderer = *renderer;
        renderer_creations++;
        printf("GL vendor=%s renderer=%s version=%s\n", glGetString(GL_VENDOR),
            glGetString(GL_RENDERER), glGetString(GL_VERSION));
    }
    return status;
}
EGLSurface __real_eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
EGLSurface __wrap_eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
    EGLNativeWindowType window, const EGLint *attributes)
{
    egl_surface_creations++;
    return __real_eglCreateWindowSurface(display, config, window, attributes);
}
#endif

static double milliseconds(clockid_t clock)
{
    struct timespec now;
    CHECK(clock_gettime(clock, &now) == 0);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

typedef struct weston_host {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_surface *parent;
    boxrig_terminal *terminal;
    bool configured;
    bool closed;
} weston_host;

typedef struct host_buffer {
    struct wl_buffer *proxy;
    void *mapping;
    size_t size;
    unsigned int width;
    unsigned int height;
    bool released;
} host_buffer;

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
    weston_host *host = data;

    if (host->compositor == NULL &&
        strcmp(interface, wl_compositor_interface.name) == 0) {
        host->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            version < 6 ? version : 6);
        wl_proxy_set_queue((struct wl_proxy *)host->compositor, host->queue);
    } else if (host->shm == NULL &&
               strcmp(interface, wl_shm_interface.name) == 0) {
        host->shm = wl_registry_bind(
            registry,
            name,
            &wl_shm_interface,
            version < 1 ? version : 1);
        wl_proxy_set_queue((struct wl_proxy *)host->shm, host->queue);
    } else if (host->wm_base == NULL &&
               strcmp(interface, xdg_wm_base_interface.name) == 0) {
        host->wm_base = wl_registry_bind(
            registry,
            name,
            &xdg_wm_base_interface,
            version < 7 ? version : 7);
        wl_proxy_set_queue((struct wl_proxy *)host->wm_base, host->queue);
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(
    void *data,
    struct xdg_wm_base *wm_base,
    uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(
    void *data,
    struct xdg_surface *surface,
    uint32_t serial)
{
    weston_host *host = data;

    xdg_surface_ack_configure(surface, serial);
    host->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(
    void *data,
    struct xdg_toplevel *toplevel,
    int32_t width,
    int32_t height,
    struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    weston_host *host = data;

    (void)toplevel;
    host->closed = true;
}

static void toplevel_configure_bounds(
    void *data,
    struct xdg_toplevel *toplevel,
    int32_t width,
    int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_wm_capabilities(
    void *data,
    struct xdg_toplevel *toplevel,
    struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static bool dispatch_once(
    weston_host *host,
    boxrig_wayland_context *context,
    int timeout_milliseconds)
{
    struct pollfd descriptor;
    boxrig_wayland_dispatch_result dispatch_result;
    int result;

    if (wl_display_dispatch_queue_pending(host->display, host->queue) < 0) {
        return false;
    }
    result = wl_display_flush(host->display);
    if (result >= 0) {
        boxrig_wayland_context_note_flushed(context);
    } else if (errno != EAGAIN) {
        return false;
    }
    while (wl_display_prepare_read_queue(host->display, host->queue) != 0) {
        if (wl_display_dispatch_queue_pending(host->display, host->queue) < 0) {
            return false;
        }
    }
    descriptor.fd = wl_display_get_fd(host->display);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, timeout_milliseconds);
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
        if (wl_display_read_events(host->display) < 0) {
            return false;
        }
    } else {
        wl_display_cancel_read(host->display);
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    if (wl_display_dispatch_queue_pending(host->display, host->queue) < 0) {
        return false;
    }
    return boxrig_wayland_context_dispatch(
               context,
               BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
               &dispatch_result) == BOXRIG_UI_OK;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    host_buffer *host_buffer = data;

    (void)buffer;
    host_buffer->released = true;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void create_host_buffer(
    weston_host *host,
    unsigned int width,
    unsigned int height,
    uint32_t color,
    host_buffer *buffer)
{
    struct wl_shm_pool *pool;
    size_t index;
    uint32_t *pixels;
    int fd;

    memset(buffer, 0, sizeof(*buffer));
    CHECK(height <= SIZE_MAX / ((size_t)width * 4u));
    buffer->size = (size_t)width * (size_t)height * 4u;
    buffer->width = width;
    buffer->height = height;
    fd = memfd_create("boxrig-weston-parent", MFD_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)buffer->size) == 0);
    buffer->mapping = mmap(
        NULL,
        buffer->size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    CHECK(buffer->mapping != MAP_FAILED);
    pixels = buffer->mapping;
    for (index = 0; index < (size_t)width * height; index++) {
        pixels[index] = color;
    }
    pool = wl_shm_create_pool(host->shm, fd, (int32_t)buffer->size);
    CHECK(pool != NULL);
    wl_proxy_set_queue((struct wl_proxy *)pool, host->queue);
    buffer->proxy = wl_shm_pool_create_buffer(
        pool,
        0,
        (int32_t)width,
        (int32_t)height,
        (int32_t)(width * 4u),
        WL_SHM_FORMAT_ARGB8888);
    CHECK(buffer->proxy != NULL);
    wl_proxy_set_queue((struct wl_proxy *)buffer->proxy, host->queue);
    CHECK(wl_buffer_add_listener(
              buffer->proxy,
              &buffer_listener,
              buffer) == 0);
    wl_shm_pool_destroy(pool);
    close(fd);
}

static void destroy_host_buffer(host_buffer *buffer)
{
    if (buffer->proxy != NULL) {
        wl_buffer_destroy(buffer->proxy);
    }
    if (buffer->mapping != NULL && buffer->mapping != MAP_FAILED) {
        munmap(buffer->mapping, buffer->size);
    }
}

static void feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
}

static void wait_for_globals(
    weston_host *host,
    boxrig_wayland_context *context)
{
    boxrig_wayland_context_diagnostic diagnostic;
    size_t attempt;

    for (attempt = 0; attempt < 500; attempt++) {
        CHECK(dispatch_once(host, context, 20));
        CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
              BOXRIG_UI_OK);
        if (host->compositor != NULL && host->shm != NULL &&
            host->wm_base != NULL &&
            diagnostic.state == BOXRIG_WAYLAND_CONTEXT_READY) {
            return;
        }
    }
    CHECK(false);
}

static void wait_for_component(
    weston_host *host,
    boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component)
{
    boxrig_ui_component_frame_result frame_result;
    boxrig_wayland_component_diagnostic diagnostic;
#if BOXRIG_TEST_WAYLAND_OPENGL
    boxrig_wayland_opengl_component_diagnostic opengl_diagnostic;
#endif
    boxrig_ui_transparency_state transparency;
    size_t attempt;

    for (attempt = 0; attempt < 500; attempt++) {
        CHECK(boxrig_wayland_terminal_component_frame(
                  component,
                  &frame_result) == BOXRIG_UI_OK);
        if (native_display) {
            boxrig_ui_layout_action grid;
            if (boxrig_wayland_terminal_component_take_layout_action(component, &grid)) {
                CHECK(grid.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
                CHECK(boxrig_terminal_resize(host->terminal, grid.value.grid_resize.rows,
                    grid.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
                    grid.revision, true) == BOXRIG_UI_OK);
            }
            boxrig_wayland_host_action action;
            if (boxrig_wayland_terminal_component_take_host_action(component, &action)) {
                bool applied;
                wl_surface_commit(host->parent);
                CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
                    action.revision, &applied) == BOXRIG_UI_OK && applied);
            }
        }
        CHECK(dispatch_once(host, context, 20));
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(
                  component,
                  &diagnostic) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_get_transparency(
                  component,
                  &transparency) == BOXRIG_UI_OK);
        if ((native_display || diagnostic.scale_numerator == 240) &&
            diagnostic.buffer_width == (diagnostic.logical_width * diagnostic.scale_numerator + 119) / 120 &&
            diagnostic.buffer_height == (diagnostic.logical_height * diagnostic.scale_numerator + 119) / 120 &&
#if BOXRIG_TEST_WAYLAND_OPENGL
            diagnostic.shm_format == 0 &&
#else
            diagnostic.shm_format == WL_SHM_FORMAT_ARGB8888 &&
#endif
            transparency.effective ==
                BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
            !diagnostic.reparent_pending && !diagnostic.frame_callback_pending &&
            diagnostic.busy_buffer_count <= 1 &&
            diagnostic.presentation_generation > 0 &&
            !diagnostic.presentation_pending && !frame_result.frame_pending) {
            CHECK((diagnostic.fractional_scale_active &&
                   diagnostic.buffer_scale == 1) ||
                  (!diagnostic.fractional_scale_active &&
                   diagnostic.buffer_scale * 120 == (int32_t)diagnostic.scale_numerator));
#if BOXRIG_TEST_WAYLAND_OPENGL
            CHECK(boxrig_wayland_opengl_terminal_component_get_diagnostic(
                      component,
                      &opengl_diagnostic) == BOXRIG_UI_OK);
            CHECK(opengl_diagnostic.context_generation > 0);
            CHECK(opengl_diagnostic.target_generation > 0);
            CHECK(opengl_diagnostic.accepted_swap_count > 0);
            CHECK(opengl_diagnostic.full_redraw_count > 0);
            CHECK(opengl_diagnostic.width == diagnostic.buffer_width);
            CHECK(opengl_diagnostic.height == diagnostic.buffer_height);
            CHECK(opengl_diagnostic.alpha_target);
            CHECK(!opengl_diagnostic.target_lost);
            CHECK(!opengl_diagnostic.partial_swap_enabled);
#endif
            return;
        }
    }
    fprintf(
        stderr,
        "component timeout: scale=%u logical=%ux%u buffer=%ux%u "
        "format=%u fractional=%d buffer-scale=%d frame=%d busy=%zu "
        "generation=%llu pending=%d transparency=%d failure=%d\n",
        diagnostic.scale_numerator,
        diagnostic.logical_width,
        diagnostic.logical_height,
        diagnostic.buffer_width,
        diagnostic.buffer_height,
        diagnostic.shm_format,
        diagnostic.fractional_scale_active,
        diagnostic.buffer_scale,
        diagnostic.frame_callback_pending,
        diagnostic.busy_buffer_count,
        (unsigned long long)diagnostic.presentation_generation,
        diagnostic.presentation_pending,
        (int)transparency.effective,
        diagnostic.failure_reason);
    CHECK(false);
}

static uint64_t animation_now;
static uint64_t animation_generation;
static bool animation_reserved;
static boxrig_ui_status animation_read_time(void *context, uint64_t *now)
{
    (void)context;
    *now = animation_now;
    return BOXRIG_UI_OK;
}
static boxrig_ui_status animation_reserve(void *context, uint64_t deadline,
    uint64_t generation)
{
    (void)context;
    CHECK(deadline > animation_now);
    animation_generation = generation;
    animation_reserved = true;
    return BOXRIG_UI_OK;
}
static void animation_cancel(void *context)
{
    (void)context;
    animation_reserved = false;
}

int main(void)
{
    native_display = getenv("BOXRIG_TEST_NATIVE_WAYLAND") != NULL;
    static const uint8_t text[] = "weston \xce\xb1\xe6\xbc\xa2";
    static const uint8_t image[] =
        "\x1b_Ga=T,i=901,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\";
    boxrig_ui_animation_poll *idle_adapter = NULL;
    int idle_timeout = 0;
    CHECK(boxrig_wayland_animation_poll_create(&idle_adapter) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_animation_poll_prepare_wait(idle_adapter, -1, &idle_timeout) == BOXRIG_UI_OK);
    CHECK(idle_timeout == -1);
    boxrig_ui_animation_poll_destroy(idle_adapter);
    weston_host host = {0};
    host_buffer background_first;
    host_buffer background_second;
    boxrig_wayland_context *context = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_ui_animation_runtime *runtime = NULL;
    boxrig_ui_animation_scheduler scheduler = {NULL, animation_read_time, animation_reserve, animation_cancel};
    CHECK(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    component_config.animation_runtime = runtime;
    struct wl_surface *parent;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_wayland_host_action action;
    boxrig_ui_hit_test_result hit;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_ui_transparency_config transparency =
        boxrig_ui_transparency_config_default();
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic diagnostic;
    const uint8_t *selected;
    size_t selected_length;
    uint64_t generation;
    size_t attempt;
    bool applied;
    boxrig_ui_status status;
    void *wrapper;

    host.display = wl_display_connect(NULL);
    CHECK(host.display != NULL);
    host.queue = wl_display_create_queue(host.display);
    CHECK(host.queue != NULL);
    wrapper = wl_proxy_create_wrapper(host.display);
    CHECK(wrapper != NULL);
    wl_proxy_set_queue(wrapper, host.queue);
    host.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    CHECK(host.registry != NULL);
    wl_proxy_set_queue((struct wl_proxy *)host.registry, host.queue);
    CHECK(wl_registry_add_listener(
              host.registry,
              &registry_listener,
              &host) == 0);
    CHECK(boxrig_wayland_context_create(
              host.display,
              host.queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    wait_for_globals(&host, context);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.xkb_available);
    if (!native_display) {
    CHECK(context_diagnostic.seat_count == 0);
    CHECK(context_diagnostic.text_input_version == 0);
    CHECK((context_diagnostic.input_capabilities &
           (BOXRIG_WAYLAND_INPUT_CAPABILITY_KEYBOARD |
            BOXRIG_WAYLAND_INPUT_CAPABILITY_POINTER |
            BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3)) == 0);
    }
    CHECK(xdg_wm_base_add_listener(
              host.wm_base,
              &wm_base_listener,
              &host) == 0);

    parent = wl_compositor_create_surface(host.compositor);
    host.parent = parent;
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, host.queue);
    host.xdg_surface = xdg_wm_base_get_xdg_surface(host.wm_base, parent);
    CHECK(host.xdg_surface != NULL);
    wl_proxy_set_queue((struct wl_proxy *)host.xdg_surface, host.queue);
    CHECK(xdg_surface_add_listener(
              host.xdg_surface,
              &xdg_surface_listener,
              &host) == 0);
    host.toplevel = xdg_surface_get_toplevel(host.xdg_surface);
    CHECK(host.toplevel != NULL);
    wl_proxy_set_queue((struct wl_proxy *)host.toplevel, host.queue);
    CHECK(xdg_toplevel_add_listener(
              host.toplevel,
              &toplevel_listener,
              &host) == 0);
    xdg_toplevel_set_title(host.toplevel, "boxrig-native-wayland-test");
    wl_surface_commit(parent);
    for (attempt = 0; attempt < 500 && !host.configured; attempt++) {
        CHECK(dispatch_once(&host, context, 20));
    }
    CHECK(host.configured && !host.closed);

    terminal_config.rows = 6;
    terminal_config.columns = 24;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    host.terminal = terminal;
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 10, 20) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    feed(terminal, text, sizeof(text) - 1);
    feed(terminal, image, sizeof(image) - 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
#if BOXRIG_TEST_WAYLAND_OPENGL
    status = boxrig_wayland_opengl_terminal_component_create(
#else
    status = boxrig_wayland_terminal_component_create(
#endif
        context,
        parent,
        model,
        &component_config,
        &component);
    CHECK(status == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 0) == BOXRIG_STATUS_BUSY);
    allocation.x = 16;
    allocation.y = 16;
    allocation.width = 240;
    allocation.height = 120;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              BOXRIG_UI_FIXED_ONE,
              BOXRIG_UI_FIXED_ONE,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid);
    CHECK(boxrig_ui_view_begin_selection(
              boxrig_wayland_terminal_component_view(component),
              BOXRIG_UI_SELECTION_WORD,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(
              boxrig_wayland_terminal_component_view(component),
              &snapshot) == BOXRIG_UI_OK);
    selected = boxrig_text_snapshot_data(snapshot, &selected_length);
    CHECK(selected_length == 6 && memcmp(selected, "weston", 6) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    snapshot = NULL;
    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.background_opacity = 128;
    CHECK(boxrig_wayland_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_OK);

    create_host_buffer(&host, 320, 180, UINT32_C(0xff203060), &background_first);
    wl_surface_attach(parent, background_first.proxy, 0, 0);
    wl_surface_damage_buffer(parent, 0, 0, 320, 180);
    xdg_surface_set_window_geometry(host.xdg_surface, 0, 0, 320, 180);
    CHECK(boxrig_wayland_terminal_component_take_host_action(
        component,
        &action));
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
              component,
              action.revision,
              &applied) == BOXRIG_UI_OK);
    CHECK(applied);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    boxrig_ui_view *animation_view = boxrig_wayland_terminal_component_view(component);
    CHECK(boxrig_ui_view_set_focused(animation_view, true) == BOXRIG_UI_OK);
    CHECK(animation_reserved);
    CHECK(boxrig_ui_view_cursor_blink_visible(animation_view));
    animation_now = 500;
    CHECK(boxrig_ui_animation_runtime_dispatch(runtime, animation_generation) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_cursor_blink_visible(animation_view));
    CHECK(boxrig_wayland_terminal_component_set_visible(component, false) == BOXRIG_UI_OK);
    CHECK(!animation_reserved);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_cursor_blink_visible(animation_view));
    CHECK(animation_reserved);
    CHECK(boxrig_ui_view_set_focused(animation_view, false) == BOXRIG_UI_OK);
    CHECK(!animation_reserved);
    wait_for_component(&host, context, component);
    weston_host moved_host = host;
    moved_host.configured = false;
    moved_host.parent = wl_compositor_create_surface(host.compositor);
    CHECK(moved_host.parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)moved_host.parent, host.queue);
    moved_host.xdg_surface = xdg_wm_base_get_xdg_surface(host.wm_base, moved_host.parent);
    CHECK(moved_host.xdg_surface != NULL);
    CHECK(xdg_surface_add_listener(moved_host.xdg_surface, &xdg_surface_listener, &moved_host) == 0);
    moved_host.toplevel = xdg_surface_get_toplevel(moved_host.xdg_surface);
    CHECK(moved_host.toplevel != NULL);
    CHECK(xdg_toplevel_add_listener(moved_host.toplevel, &toplevel_listener, &moved_host) == 0);
    wl_surface_commit(moved_host.parent);
    for (attempt = 0; attempt < 500 && !moved_host.configured; attempt++)
        CHECK(dispatch_once(&host, context, 20));
    CHECK(moved_host.configured);
    host_buffer moved_background;
    create_host_buffer(&host, 320, 180, UINT32_C(0xff203060), &moved_background);
    wl_surface_attach(moved_host.parent, moved_background.proxy, 0, 0);
    wl_surface_damage_buffer(moved_host.parent, 0, 0, 320, 180);
    wl_surface_commit(moved_host.parent);
    boxrig_ui_view *retained_view = boxrig_wayland_terminal_component_view(component);
    struct wl_surface *retained_surface = boxrig_wayland_terminal_component_surface(component);
    compare_reparent_frames = true;
    boxrig_ui_dirty move_dirty = {.row_count = 6, .column_count = 24};
    CHECK(boxrig_ui_view_invalidate(retained_view, BOXRIG_UI_DIRTY_PRESENT, move_dirty) == BOXRIG_UI_OK);
    wait_for_component(&host, context, component);
    CHECK(reparent_frame_count > 0);
    uint64_t retained_frame_hash = reparent_frame_hash;
#if BOXRIG_TEST_WAYLAND_OPENGL
    size_t retained_renderers = renderer_creations;
    size_t retained_egl_surfaces = egl_surface_creations;
    boxrig_wayland_opengl_component_diagnostic retained_gl;
    CHECK(boxrig_wayland_opengl_terminal_component_get_diagnostic(component, &retained_gl) == BOXRIG_UI_OK);
#endif
    uint64_t last_move = 0;
    for (unsigned int step = 0; step < 4; step++) {
        size_t previous_frames = reparent_frame_count;
        struct wl_surface *destination = step % 2 == 0 ? moved_host.parent : parent;
        uint64_t move_revision = 0;
        CHECK(boxrig_wayland_terminal_component_request_reparent(component, destination, &move_revision) == BOXRIG_UI_OK);
        CHECK(move_revision > last_move);
        last_move = move_revision;
        CHECK(boxrig_wayland_terminal_component_view(component) == retained_view);
        CHECK(boxrig_wayland_terminal_component_surface(component) == retained_surface);
        CHECK(boxrig_ui_view_is_layout_authority(retained_view));
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && !applied);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(diagnostic.reparent_pending && diagnostic.effective_reparent_revision < move_revision);
        boxrig_ui_component_frame_result move_frame;
        CHECK(boxrig_wayland_terminal_component_frame(component, &move_frame) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
        CHECK(action.parent == destination);
        wl_surface_commit(action.parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component, action.revision, &applied) == BOXRIG_UI_OK && applied);
        wait_for_component(&host, context, component);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(!diagnostic.reparent_pending && diagnostic.effective_reparent_revision == move_revision);
        CHECK(reparent_frame_count > previous_frames);
        CHECK(reparent_frame_hash == retained_frame_hash);
#if BOXRIG_TEST_WAYLAND_OPENGL
        boxrig_wayland_opengl_component_diagnostic moved_gl;
        CHECK(boxrig_wayland_opengl_terminal_component_get_diagnostic(component, &moved_gl) == BOXRIG_UI_OK);
        CHECK(renderer_creations == retained_renderers && egl_surface_creations == retained_egl_surfaces);
        CHECK(moved_gl.context_generation == retained_gl.context_generation);
        CHECK(moved_gl.target_generation == retained_gl.target_generation);
#endif
        CHECK(boxrig_ui_view_create_selection_snapshot(retained_view, &snapshot) == BOXRIG_UI_OK);
        selected = boxrig_text_snapshot_data(snapshot, &selected_length);
        CHECK(selected_length == 6 && memcmp(selected, "weston", 6) == 0);
        boxrig_text_snapshot_destroy(snapshot);
        snapshot = NULL;
    }
    compare_reparent_frames = false;
    xdg_toplevel_destroy(moved_host.toplevel);
    xdg_surface_destroy(moved_host.xdg_surface);
    wl_surface_destroy(moved_host.parent);
    destroy_host_buffer(&moved_background);

    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    generation = diagnostic.presentation_generation;

    /* Composition-only edits must advance a frame without terminal dirty. */
    boxrig_composition *composition =
        boxrig_wayland_terminal_component_composition(component);
    boxrig_composition_event preedit = {0};
    boxrig_composition_view composition_view;
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_composition_focus_changed(composition, true) ==
          BOXRIG_COMPOSITION_OK);
    preedit.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    preedit.text = "abcd";
    preedit.text_length = 4;
    preedit.caret_visible = true;
    for (size_t caret = 4; caret >= 2; caret--) {
        CHECK(!boxrig_ui_view_frame_requested(
            boxrig_wayland_terminal_component_view(component)));
        preedit.caret_offset = caret;
        CHECK(boxrig_composition_apply_event(composition, &preedit) ==
              BOXRIG_COMPOSITION_OK);
        wait_for_component(&host, context, component);
        CHECK(boxrig_composition_get_view(composition, &composition_view) ==
              BOXRIG_COMPOSITION_OK);
        CHECK(composition_view.candidate.visible);
        CHECK(composition_view.candidate.spot_x ==
              (composition_view.anchor_visual_column + caret) * metrics.cell_width);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(
                  component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(diagnostic.presentation_generation > generation);
        generation = diagnostic.presentation_generation;
    }
    /* A synchronized-output hold must retain an IME-only edit until release. */
    feed(terminal, (const uint8_t *)"\033[?2026h", 8);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    preedit.caret_offset = 1;
    CHECK(boxrig_composition_apply_event(composition, &preedit) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_ui_component_frame_result held_frame;
    CHECK(boxrig_wayland_terminal_component_frame(component, &held_frame) ==
          BOXRIG_UI_OK);
    CHECK(held_frame.frame_pending);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == generation);
    feed(terminal, (const uint8_t *)"\033[?2026l", 8);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    wait_for_component(&host, context, component);
    CHECK(boxrig_composition_get_view(composition, &composition_view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.candidate.spot_x ==
          (composition_view.anchor_visual_column + 1) * metrics.cell_width);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation > generation);
    generation = diagnostic.presentation_generation;
    memset(&preedit, 0, sizeof(preedit));
    preedit.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &preedit) ==
          BOXRIG_COMPOSITION_OK);
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation > generation);
    generation = diagnostic.presentation_generation;

    create_host_buffer(&host, 320, 180, UINT32_C(0xff306020), &background_second);
    wl_surface_attach(parent, background_second.proxy, 0, 0);
    wl_surface_damage_buffer(parent, 0, 0, 320, 180);
    wl_surface_commit(parent);
    for (attempt = 0; attempt < 20; attempt++) {
        CHECK(dispatch_once(&host, context, 20));
    }
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == generation);
    CHECK(!host.closed);

    /* Exercise the public standard bar through both software and EGL hosts. */
    boxrig_ui_layout_action grid_action;
    if (boxrig_wayland_terminal_component_take_layout_action(component, &grid_action)) {
        CHECK(grid_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, grid_action.value.grid_resize.rows,
            grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
            grid_action.revision, true) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    for (unsigned int cycle = 0; cycle < 3; cycle++) {
        boxrig_ui_scrollbar_visibility requested = cycle == 1 ? BOXRIG_UI_SCROLLBAR_ALWAYS
            : BOXRIG_UI_SCROLLBAR_NEVER;
        CHECK(boxrig_wayland_terminal_component_set_scrollbar_visibility(component, requested) == BOXRIG_UI_OK);
        boxrig_ui_scrollbar_visibility_state bar;
        CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component, &bar) == BOXRIG_UI_OK);
        CHECK(bar.pending && bar.requested == requested);
        CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &grid_action));
        CHECK(grid_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, grid_action.value.grid_resize.rows,
            grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
            grid_action.revision, true) == BOXRIG_UI_OK);
        CHECK(!boxrig_wayland_terminal_component_take_layout_action(component, &grid_action));
        CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
        wl_surface_commit(parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
            action.revision, &applied) == BOXRIG_UI_OK && applied);
        wait_for_component(&host, context, component);
        CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component, &bar) == BOXRIG_UI_OK);
        CHECK(!bar.pending && bar.visible == (requested == BOXRIG_UI_SCROLLBAR_ALWAYS));
        CHECK(bar.reserved_width == (bar.visible ? 14u : 0u));
        CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(boxrig_wayland_terminal_component_get_layout(component, &allocation, &layout) == BOXRIG_UI_OK);
        CHECK(layout.columns == (diagnostic.buffer_width - (bar.reserved_width * diagnostic.scale_numerator + 119) / 120) / metrics.cell_width);
    }
    printf("PASS native Wayland standard bar visibility at scale %u/120\n", diagnostic.scale_numerator);

    if (getenv("BOXRIG_TEST_RESIZE_HISTORY") != NULL) {
        for (size_t line = 0; line < 10050; line++) {
            feed(terminal, text, sizeof(text) - 1);
            feed(terminal, (const uint8_t *)"\r\n", 2);
        }
        CHECK(boxrig_terminal_scrollback_rows(terminal) == 10000);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        wait_for_component(&host, context, component);
        printf("HISTORY rows=%zu\n", boxrig_terminal_scrollback_rows(terminal));
    }

    /* Allocation work is measured separately from dispatch/compositor latency.
     * Repeated dimensions must keep the already accepted image and resources. */
    const unsigned int resize_sizes[][2] = {{640, 360}, {1000, 650}, {1000, 650},
        {480, 280}, {480, 280}, {800, 500}, {640, 360}, {640, 360}};
    bool height_only = getenv("BOXRIG_TEST_RESIZE_HEIGHT_ONLY") != NULL;
    host_buffer resize_background = {0};
    capture_active = true;
    for (size_t cycle = 0; cycle < 4; cycle++) {
        for (size_t index = 0; index < sizeof(resize_sizes) / sizeof(resize_sizes[0]); index++) {
            boxrig_wayland_component_diagnostic before;
            CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &before) == BOXRIG_UI_OK);
            unsigned int target_width = height_only ? 640 : resize_sizes[index][0];
            bool same = allocation.width == target_width && allocation.height == resize_sizes[index][1];
            allocation.width = target_width;
            allocation.height = resize_sizes[index][1];
#if BOXRIG_TEST_WAYLAND_OPENGL
            size_t renderers_before = renderer_creations;
            size_t surfaces_before = egl_surface_creations;
            boxrig_ogl_render_statistics stats_before, stats_after;
            CHECK(boxrig_ogl_renderer_get_render_statistics(observed_renderer, &stats_before) == 0);
#endif
#if !BOXRIG_TEST_WAYLAND_OPENGL
            size_t copies_before = copied_buffers;
#endif
            double resize_start = milliseconds(CLOCK_MONOTONIC);
            double cpu = milliseconds(CLOCK_THREAD_CPUTIME_ID);
            double wall = milliseconds(CLOCK_MONOTONIC);
            CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
            wall = milliseconds(CLOCK_MONOTONIC) - wall;
            cpu = milliseconds(CLOCK_THREAD_CPUTIME_ID) - cpu;
            CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
            printf("RESIZE cycle=%zu index=%zu size=%ux%u same=%d wall_ms=%.6f cpu_ms=%.6f presentations=%llu\n",
                cycle, index, allocation.width, allocation.height, same, wall, cpu,
                (unsigned long long)(diagnostic.presentation_generation - before.presentation_generation));
            fflush(stdout);
#if !BOXRIG_TEST_WAYLAND_OPENGL
            printf("COPIES cycle=%zu index=%zu count=%zu\n", cycle, index, copied_buffers - copies_before);
            if (getenv("BOXRIG_TEST_RESIZE_MEASURE") == NULL)
                CHECK(copied_buffers - copies_before == (same ? 0u : 1u));
#endif
#if BOXRIG_TEST_WAYLAND_OPENGL
            CHECK(renderer_creations == renderers_before);
            CHECK(egl_surface_creations == surfaces_before);
            CHECK(boxrig_ogl_renderer_get_render_statistics(observed_renderer, &stats_after) == 0);
            CHECK(stats_after.grayscale_glyph_upload_count == stats_before.grayscale_glyph_upload_count);
            CHECK(stats_after.color_glyph_upload_count == stats_before.color_glyph_upload_count);
#endif
            if (getenv("BOXRIG_TEST_RESIZE_MEASURE") == NULL && same) {
                CHECK(diagnostic.presentation_generation == before.presentation_generation);
            }
            double core_ms = 0;
            if (boxrig_wayland_terminal_component_take_layout_action(component, &grid_action)) {
                double start = milliseconds(CLOCK_MONOTONIC);
                CHECK(boxrig_terminal_resize(terminal, grid_action.value.grid_resize.rows,
                    grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
                core_ms = milliseconds(CLOCK_MONOTONIC) - start;
                CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
                    grid_action.revision, true) == BOXRIG_UI_OK);
            }
            double synchronize_start = milliseconds(CLOCK_MONOTONIC);
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            double synchronize_ms = milliseconds(CLOCK_MONOTONIC) - synchronize_start;
            host_buffer previous_background = resize_background;
            create_host_buffer(&host, allocation.width + 32, allocation.height + 32,
                UINT32_C(0xff203060), &resize_background);
            wl_surface_attach(parent, resize_background.proxy, 0, 0);
            wl_surface_damage_buffer(parent, 0, 0, (int32_t)resize_background.width, (int32_t)resize_background.height);
            xdg_surface_set_window_geometry(host.xdg_surface, 0, 0,
                (int32_t)resize_background.width, (int32_t)resize_background.height);
            bool has_action = boxrig_wayland_terminal_component_take_host_action(component, &action);
            double parent_start = milliseconds(CLOCK_MONOTONIC);
            wl_surface_commit(parent);
            if (has_action) CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
                action.revision, &applied) == BOXRIG_UI_OK && applied);
            double parent_ms = milliseconds(CLOCK_MONOTONIC) - parent_start;
            /* Settling includes the post-resize frame and compositor callbacks;
             * allocation alone can submit an image before the core is resized. */
            wait_for_component(&host, context, component);
            double settled_ms = milliseconds(CLOCK_MONOTONIC) - resize_start;
            size_t final_rows, final_columns;
            boxrig_ui_component_allocation final_allocation;
            boxrig_ui_layout_result final_layout;
            boxrig_terminal_size(terminal, &final_rows, &final_columns);
            CHECK(boxrig_wayland_terminal_component_get_layout(component,
                &final_allocation, &final_layout) == BOXRIG_UI_OK);
            CHECK(final_rows == final_layout.rows && final_columns == final_layout.columns);
            printf("RESIZE_COMPLETE cycle=%zu index=%zu height_only=%d same=%d rows=%zu columns=%zu core_ms=%.6f synchronize_ms=%.6f parent_ms=%.6f settled_ms=%.6f\n",
                cycle, index, height_only, same, final_rows, final_columns,
                core_ms, synchronize_ms, parent_ms, settled_ms);
            destroy_host_buffer(&previous_background);
        }
    }
    capture_active = false;

    /* Moving an accepted image only requires the parent's position commit. */
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    generation = diagnostic.presentation_generation;
    allocation.x++;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    CHECK(action.x == allocation.x && action.y == allocation.y);
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        action.revision, &applied) == BOXRIG_UI_OK && applied);
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    if (getenv("BOXRIG_TEST_RESIZE_MEASURE") == NULL)
        CHECK(diagnostic.presentation_generation == generation);

    /* A same-size allocation with new content still prepares the new image. */
    feed(terminal, (const uint8_t *)"\rZ", 2);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    generation = diagnostic.presentation_generation;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        action.revision, &applied) == BOXRIG_UI_OK && applied);
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation > generation);

    boxrig_ui_component_allocation invalid = allocation;
    invalid.width = UINT32_MAX;
    generation = diagnostic.presentation_generation;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &invalid, &layout) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == generation);
    CHECK(diagnostic.logical_width == allocation.width && diagnostic.logical_height == allocation.height);

    /* A failure within the same terminal grid must still restore the old
     * allocation's excess pixels when the host retries its accepted size. */
    boxrig_ui_component_allocation failed_allocation = allocation;
    failed_allocation.width++;
    failed_allocation.height++;
    fail_next_presentation = true;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &failed_allocation, &layout) == BOXRIG_UI_UNAVAILABLE);
    CHECK(!fail_next_presentation);
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_layout_result view_layout;
    CHECK(boxrig_ui_view_get_layout(boxrig_wayland_terminal_component_view(component), &view_layout) == BOXRIG_UI_OK);
    CHECK(view_layout.excess_right == layout.excess_right && view_layout.excess_bottom == layout.excess_bottom);
    if (boxrig_wayland_terminal_component_take_host_action(component, &action)) {
        wl_surface_commit(parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
            action.revision, &applied) == BOXRIG_UI_OK && applied);
    }
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    generation = diagnostic.presentation_generation;

    /* A failed resized presentation retains the accepted dimensions and
     * accepts a subsequent retry of the new allocation. */
    allocation.width += 10;
    allocation.height += 10;
    fail_next_presentation = true;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_UNAVAILABLE);
    CHECK(!fail_next_presentation);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == generation);
    CHECK(diagnostic.logical_width + 10 == allocation.width && diagnostic.logical_height + 10 == allocation.height);
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    if (boxrig_wayland_terminal_component_take_layout_action(component, &grid_action)) {
        CHECK(boxrig_terminal_resize(terminal, grid_action.value.grid_resize.rows,
            grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
            grid_action.revision, true) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        action.revision, &applied) == BOXRIG_UI_OK && applied);
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.logical_width == allocation.width && diagnostic.logical_height == allocation.height);
    CHECK(diagnostic.presentation_generation > generation);
    puts("PASS Wayland resize resources, position changes, pending paint, and allocation limits");

    boxrig_wayland_terminal_component_destroy(component);
    component = NULL;
    CHECK(!animation_reserved);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 500) == BOXRIG_STATUS_OK);
    boxrig_ui_animation_runtime_destroy(runtime);
    boxrig_wayland_context_destroy(context);
    context = NULL;
    xdg_toplevel_destroy(host.toplevel);
    xdg_surface_destroy(host.xdg_surface);
    wl_surface_destroy(parent);
    destroy_host_buffer(&resize_background);
    destroy_host_buffer(&background_second);
    destroy_host_buffer(&background_first);
    xdg_wm_base_destroy(host.wm_base);
    wl_shm_destroy(host.shm);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host.registry);
    CHECK(wl_display_flush(host.display) >= 0);
    wl_event_queue_destroy(host.queue);
    wl_display_disconnect(host.display);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    FcFini();
    printf(
#if BOXRIG_TEST_WAYLAND_OPENGL
        "PASS native Wayland OpenGL component renders\n"
#else
        "PASS native Wayland component renders\n"
#endif
    );
    return 0;
}
