/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define GL_GLEXT_PROTOTYPES 1

#include "boxrig/render/opengl.h"
#include "boxrig/terminal.h"
#include "boxrig/ui.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TARGET_WIDTH 320u
#define TARGET_HEIGHT 128u

typedef struct example_gl_context {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
} example_gl_context;

static boxrig_gl_function resolve_gl_function(void *context, const char *name)
{
    return context == NULL || name == NULL
               ? NULL
               : (boxrig_gl_function)eglGetProcAddress(name);
}

static bool query_current_context(
    void *context,
    boxrig_gl_current_context *current_out)
{
    const example_gl_context *gl_context = context;
    EGLContext current;

    if (gl_context == NULL || current_out == NULL) {
        return false;
    }
    current = eglGetCurrentContext();
    current_out->identity = (uintptr_t)current;
    /* The example is deliberately single-threaded. */
    current_out->thread_identity = 1;
    return current != EGL_NO_CONTEXT && current == gl_context->context;
}

static bool create_context(example_gl_context *context)
{
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE};
    static const EGLint surface_attributes[] = {
        EGL_WIDTH,
        1,
        EGL_HEIGHT,
        1,
        EGL_NONE};
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION,
        3,
        EGL_CONTEXT_MINOR_VERSION,
        3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};
    EGLConfig config;
    EGLint count = 0;

    memset(context, 0, sizeof(*context));
    context->display = EGL_NO_DISPLAY;
    context->surface = EGL_NO_SURFACE;
    context->context = EGL_NO_CONTEXT;
    context->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (context->display == EGL_NO_DISPLAY ||
        eglInitialize(context->display, NULL, NULL) != EGL_TRUE ||
        eglBindAPI(EGL_OPENGL_API) != EGL_TRUE ||
        eglChooseConfig(
            context->display,
            config_attributes,
            &config,
            1,
            &count) != EGL_TRUE ||
        count != 1) {
        return false;
    }
    context->surface = eglCreatePbufferSurface(
        context->display,
        config,
        surface_attributes);
    context->context = eglCreateContext(
        context->display,
        config,
        EGL_NO_CONTEXT,
        context_attributes);
    return context->surface != EGL_NO_SURFACE &&
           context->context != EGL_NO_CONTEXT &&
           eglMakeCurrent(
               context->display,
               context->surface,
               context->surface,
               context->context) == EGL_TRUE;
}

static void destroy_context(example_gl_context *context)
{
    if (context->display == EGL_NO_DISPLAY) {
        return;
    }
    (void)eglMakeCurrent(
        context->display,
        EGL_NO_SURFACE,
        EGL_NO_SURFACE,
        EGL_NO_CONTEXT);
    if (context->context != EGL_NO_CONTEXT) {
        (void)eglDestroyContext(context->display, context->context);
    }
    if (context->surface != EGL_NO_SURFACE) {
        (void)eglDestroySurface(context->display, context->surface);
    }
    (void)eglTerminate(context->display);
}

static bool create_target(uint32_t *framebuffer_out, uint32_t *texture_out)
{
    GLint previous_draw;
    GLint previous_read;
    GLint previous_texture;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    bool complete;

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
        GL_RGBA8,
        TARGET_WIDTH,
        TARGET_HEIGHT,
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
    complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                   GL_FRAMEBUFFER_COMPLETE &&
               glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    if (!complete) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        return false;
    }
    *framebuffer_out = framebuffer;
    *texture_out = texture;
    return true;
}

static bool read_top_left_pixel(
    uint32_t framebuffer,
    unsigned int x,
    unsigned int y,
    uint8_t rgba_out[4])
{
    GLint previous_read;
    GLenum error;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadPixels(
        (GLint)x,
        (GLint)(TARGET_HEIGHT - y - 1u),
        1,
        1,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba_out);
    error = glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    return error == GL_NO_ERROR;
}

int main(void)
{
    static const char content[] = "\033[41m \033[0m";
    example_gl_context gl_context;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ogl_renderer_config renderer_config =
        boxrig_ogl_renderer_config_default();
    boxrig_gl_diagnostic diagnostic;
    boxrig_renderer_target lifecycle_target;
    boxrig_renderer_frame_token frame_token;
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ogl_renderer *renderer = NULL;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    uint32_t framebuffer = 0;
    uint32_t texture = 0;
    uint8_t pixel[4];
    size_t consumed = 0;
    unsigned int cell_width;
    unsigned int cell_height;
    int result = 1;

    terminal_config.rows = 1;
    terminal_config.columns = 4;
    if (!create_context(&gl_context) ||
        !create_target(&framebuffer, &texture)) {
        fprintf(stderr, "OpenGL 3.3 EGL context or FBO is unavailable\n");
        goto cleanup;
    }
    memset(&renderer_config.initial_target, 0, sizeof(renderer_config.initial_target));
    renderer_config.runtime.resolver_context = &gl_context;
    renderer_config.runtime.resolve = resolve_gl_function;
    renderer_config.runtime.current_context_query_context = &gl_context;
    renderer_config.runtime.query_current_context = query_current_context;
    renderer_config.runtime.context_generation = 1;
    renderer_config.runtime.target_generation = 1;
    renderer_config.initial_target.draw_framebuffer = framebuffer;
    renderer_config.initial_target.width = TARGET_WIDTH;
    renderer_config.initial_target.height = TARGET_HEIGHT;
    renderer_config.initial_target.generation = 1;
    renderer_config.initial_target.format = BOXRIG_GL_TARGET_RGBA8;
    renderer_config.initial_target.alpha_mode =
        BOXRIG_GL_ALPHA_PREMULTIPLIED;
    renderer_config.initial_target.origin =
        BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    memset(&diagnostic, 0, sizeof(diagnostic));
    if (boxrig_ogl_renderer_create(
            &renderer_config,
            &renderer,
            &diagnostic) != BOXRIG_GL_OK) {
        fprintf(stderr, "renderer creation failed: %s\n", diagnostic.message);
        goto cleanup;
    }
    cell_width = boxrig_ogl_renderer_cell_width(renderer);
    cell_height = boxrig_ogl_renderer_cell_height(renderer);
    if (cell_width == 0 || cell_height == 0 || cell_width > TARGET_WIDTH ||
        cell_height > TARGET_HEIGHT ||
        boxrig_terminal_create(&terminal_config, &terminal) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(
            terminal,
            cell_width,
            cell_height) != BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(
            terminal,
            (const uint8_t *)content,
            sizeof(content) - 1u,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != sizeof(content) - 1u ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        boxrig_ui_view_acquire_layout_authority(view) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width =
        (boxrig_ui_fixed)cell_width * terminal_config.columns *
        BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height =
        (boxrig_ui_fixed)cell_height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width =
        (boxrig_ui_fixed)cell_width * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height =
        (boxrig_ui_fixed)cell_height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    if (boxrig_ui_view_accept_allocation(view, &allocation, &layout) !=
            BOXRIG_UI_OK ||
        boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK ||
        layout.rows != terminal_config.rows ||
        layout.columns != terminal_config.columns) {
        goto cleanup;
    }
    memset(&lifecycle_target, 0, sizeof(lifecycle_target));
    lifecycle_target.generation = 1;
    lifecycle_target.width = TARGET_WIDTH;
    lifecycle_target.height = TARGET_HEIGHT;
    lifecycle_target.scale_numerator = 1;
    lifecycle_target.scale_denominator = 1;
    lifecycle_target.premultiplied_alpha = true;
    if (boxrig_ogl_renderer_lifecycle_create(
            renderer,
            &lifecycle_target,
            &lifecycle) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_prepare_frame(
            lifecycle,
            view,
            NULL,
            lifecycle_target.generation,
            &frame_token) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_accept_frame(lifecycle, &frame_token) !=
            BOXRIG_RENDERER_OK ||
        !read_top_left_pixel(
            framebuffer,
            cell_width / 2u,
            cell_height / 2u,
            pixel) ||
        pixel[0] <= pixel[1] || pixel[0] <= pixel[2] || pixel[3] != 255) {
        goto cleanup;
    }
    printf(
        "PASS renderer-only OpenGL frame accepted into caller FBO "
        "(%u,%u,%u,%u)\n",
        pixel[0],
        pixel[1],
        pixel[2],
        pixel[3]);
    result = 0;

cleanup:
    boxrig_renderer_lifecycle_destroy(lifecycle);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (renderer != NULL &&
        boxrig_ogl_renderer_destroy(&renderer, 1, NULL) != BOXRIG_GL_OK) {
        result = 1;
    }
    if (framebuffer != 0) {
        glDeleteFramebuffers(1, &framebuffer);
    }
    if (texture != 0) {
        glDeleteTextures(1, &texture);
    }
    destroy_context(&gl_context);
    return result;
}
