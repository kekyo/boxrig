/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "gl_area.h"

#include <epoxy/gl.h>
#include <stdio.h>
#include <string.h>

#if GTK_MAJOR_VERSION == 3
static GdkGLContext *create_context(GtkGLArea *area, gpointer data)
{
    (void)data;
    GError *error = NULL;
    GdkGLContext *previous = gdk_gl_context_get_current();
    if (previous != NULL) g_object_ref(previous);
    bool prefer_es = gtk_gl_area_get_use_es(area);
    for (unsigned int attempt = prefer_es ? 0 : 1; attempt < 2; attempt++) {
        bool use_es = attempt == 0;
        g_clear_error(&error);
        GdkGLContext *context = gdk_window_create_gl_context(
            gtk_widget_get_window(GTK_WIDGET(area)), &error);
        if (context == NULL) continue;
        gdk_gl_context_set_use_es(context, use_es);
        gdk_gl_context_set_required_version(context, 3, use_es ? 0 : 3);
        bool usable = gdk_gl_context_realize(context, &error);
        if (usable) {
            gdk_gl_context_make_current(context);
            usable = gdk_gl_context_get_current() == context;
            if (usable) {
                /* GDK may choose the API of its shared paint context instead
                 * of the requested one. Check the actual API and version. */
                bool actual_es = !epoxy_is_desktop_gl();
                int version = epoxy_gl_version();
                GLint profile = 0;
                if (!actual_es && version >= 33)
                    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
                usable = (use_es || !actual_es) &&
                    version >= (actual_es ? 30 : 33) &&
                    (actual_es || (profile & GL_CONTEXT_CORE_PROFILE_BIT) != 0);
            }
        }
        if (previous != NULL) gdk_gl_context_make_current(previous);
        else gdk_gl_context_clear_current();
        if (usable) {
            g_clear_object(&previous);
            g_clear_error(&error);
            return context;
        }
        g_object_unref(context);
    }
    g_clear_object(&previous);
    if (error == NULL) error = g_error_new_literal(GDK_GL_ERROR,
        GDK_GL_ERROR_NOT_AVAILABLE, "OpenGL ES 3.0 or OpenGL 3.3 core is unavailable");
    gtk_gl_area_set_error(area, error);
    g_error_free(error);
    g_signal_stop_emission_by_name(area, "create-context");
    return NULL;
}
#endif

GtkWidget *boxrig_gtk_gl_area_new(void)
{
    GtkWidget *widget = gtk_gl_area_new();
    GtkGLArea *area = GTK_GL_AREA(widget);
#if GTK_MAJOR_VERSION == 3
    gtk_gl_area_set_required_version(area, 3, 0);
    gtk_gl_area_set_use_es(area, TRUE);
    gtk_gl_area_set_has_alpha(area, TRUE);
    g_signal_connect(area, "create-context", G_CALLBACK(create_context), NULL);
#else
    /* Let GTK choose an API compatible with its shared context. The realized
     * context is checked for desktop GL 3.3 core or GLES 3.0 before drawing. */
    gtk_gl_area_set_required_version(area, 3, 0);
    gtk_gl_area_set_allowed_apis(area, GDK_GL_API_GL | GDK_GL_API_GLES);
#endif
    gtk_gl_area_set_auto_render(area, FALSE);
    gtk_gl_area_set_has_depth_buffer(area, FALSE);
    gtk_gl_area_set_has_stencil_buffer(area, FALSE);
    gtk_widget_set_can_focus(widget, FALSE);
    return widget;
}

boxrig_gl_function boxrig_gtk_gl_area_resolve(void *context, const char *name)
{
    (void)context;
    if (name == NULL) return NULL;
    /* libepoxy's documented gl.h dispatch pointers work for both X11 and
     * Wayland. Do not use its private resolver or a native WSI entry point. */
#define RESOLVE(function) \
    if (strcmp(name, #function) == 0) return (boxrig_gl_function)function
    RESOLVE(glGetString);
    RESOLVE(glGetStringi);
    RESOLVE(glGetIntegerv);
    RESOLVE(glGetBooleanv);
    RESOLVE(glGetFloatv);
    RESOLVE(glIsEnabled);
    RESOLVE(glEnable);
    RESOLVE(glDisable);
    RESOLVE(glViewport);
    RESOLVE(glScissor);
    RESOLVE(glColorMask);
    RESOLVE(glBlendEquationSeparate);
    RESOLVE(glBlendFuncSeparate);
    RESOLVE(glBlendColor);
    RESOLVE(glDepthMask);
    RESOLVE(glUseProgram);
    RESOLVE(glBindVertexArray);
    RESOLVE(glBindBuffer);
    RESOLVE(glActiveTexture);
    RESOLVE(glBindTexture);
    RESOLVE(glBindSampler);
    RESOLVE(glPixelStorei);
    RESOLVE(glBindFramebuffer);
    RESOLVE(glCheckFramebufferStatus);
    RESOLVE(glGetFramebufferAttachmentParameteriv);
    RESOLVE(glGetError);
    RESOLVE(glCreateShader);
    RESOLVE(glShaderSource);
    RESOLVE(glCompileShader);
    RESOLVE(glGetShaderiv);
    RESOLVE(glGetShaderInfoLog);
    RESOLVE(glDeleteShader);
    RESOLVE(glCreateProgram);
    RESOLVE(glAttachShader);
    RESOLVE(glLinkProgram);
    RESOLVE(glGetProgramiv);
    RESOLVE(glGetProgramInfoLog);
    RESOLVE(glDeleteProgram);
    RESOLVE(glGenVertexArrays);
    RESOLVE(glDeleteVertexArrays);
    RESOLVE(glGenBuffers);
    RESOLVE(glDeleteBuffers);
    RESOLVE(glBufferData);
    RESOLVE(glBufferSubData);
    RESOLVE(glGenTextures);
    RESOLVE(glDeleteTextures);
    RESOLVE(glTexImage2D);
    RESOLVE(glTexSubImage2D);
    RESOLVE(glTexParameteri);
    RESOLVE(glGenSamplers);
    RESOLVE(glDeleteSamplers);
    RESOLVE(glSamplerParameteri);
    RESOLVE(glFramebufferTexture2D);
    RESOLVE(glGetUniformLocation);
    RESOLVE(glUniform1i);
    RESOLVE(glUniform4fv);
    RESOLVE(glVertexAttribPointer);
    RESOLVE(glEnableVertexAttribArray);
    RESOLVE(glVertexAttribDivisor);
    RESOLVE(glDrawArraysInstanced);
    RESOLVE(glFenceSync);
    RESOLVE(glClientWaitSync);
    RESOLVE(glDeleteSync);
    /* The renderer queries these only after testing the matching capability. */
    RESOLVE(glGetGraphicsResetStatus);
    RESOLVE(glGetGraphicsResetStatusARB);
    RESOLVE(glGetGraphicsResetStatusKHR);
    RESOLVE(glGetDebugMessageLog);
#undef RESOLVE
    return NULL;
}

static bool current_context(void *context, boxrig_gl_current_context *current)
{
    GtkGLArea *area = context;
    GdkGLContext *actual = gdk_gl_context_get_current();
    if (actual == NULL || actual != gtk_gl_area_get_context(area)) return false;
    current->identity = (uintptr_t)actual;
    current->thread_identity = (uint64_t)(uintptr_t)g_thread_self();
    return true;
}

void boxrig_gtk_gl_area_record_error(boxrig_gtk_render_state *state,
    boxrig_gtk_render_reason reason, boxrig_ui_status status, const char *message)
{
    state->reason = reason;
    state->status = status;
    state->gl_code = 0;
    state->reset_status = 0;
    snprintf(state->message, sizeof(state->message), "%s", message);
}

void boxrig_gtk_gl_area_record_diagnostic(boxrig_gtk_render_state *state,
    const boxrig_gl_diagnostic *diagnostic)
{
    boxrig_gtk_render_reason reason = BOXRIG_GTK_RENDER_REASON_RESOURCE;
    boxrig_ui_status status = BOXRIG_UI_UNAVAILABLE;
    if (diagnostic->status == BOXRIG_GL_UNAVAILABLE ||
        diagnostic->status == BOXRIG_GL_TARGET_INCOMPLETE ||
        diagnostic->status == BOXRIG_GL_TARGET_MISMATCH) {
        reason = BOXRIG_GTK_RENDER_REASON_CAPABILITY;
    } else if (diagnostic->status == BOXRIG_GL_CONTEXT_MISMATCH ||
        diagnostic->status == BOXRIG_GL_STALE_GENERATION || diagnostic->reset_status != 0) {
        reason = BOXRIG_GTK_RENDER_REASON_CONTEXT_LOST;
    } else if (diagnostic->status == BOXRIG_GL_INVALID_ARGUMENT) {
        status = BOXRIG_UI_INVALID_ARGUMENT;
    } else if (diagnostic->status == BOXRIG_GL_OUT_OF_MEMORY) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
    }
    boxrig_gtk_gl_area_record_error(state, reason, status, diagnostic->message);
    state->gl_code = diagnostic->gl_code;
    state->reset_status = diagnostic->reset_status;
}

boxrig_ui_status boxrig_gtk_gl_area_describe(
    GtkGLArea *area, uint64_t context_generation, uint64_t target_generation,
    boxrig_gl_runtime_config *runtime, boxrig_gtk_gl_destination *destination,
    boxrig_gtk_render_state *state)
{
    if (area == NULL || runtime == NULL || destination == NULL || state == NULL ||
        context_generation == 0 || target_generation == 0) return BOXRIG_UI_INVALID_ARGUMENT;
    const GError *error = gtk_gl_area_get_error(area);
    boxrig_gl_current_context current;
    if (error != NULL || !current_context(area, &current)) {
        boxrig_gtk_gl_area_record_error(state, BOXRIG_GTK_RENDER_REASON_CONTEXT,
            BOXRIG_UI_UNAVAILABLE, error != NULL ? error->message : "GTK GL context is not current");
        return state->status;
    }
    bool is_gles = !epoxy_is_desktop_gl();
    if (epoxy_gl_version() < (is_gles ? 30 : 33)) {
        boxrig_gtk_gl_area_record_error(state, BOXRIG_GTK_RENDER_REASON_CAPABILITY,
            BOXRIG_UI_UNAVAILABLE, "OpenGL 3.3 core or OpenGL ES 3.0 is required");
        return state->status;
    }
    GLint profile = 0, framebuffer = 0, samples = 0, viewport[4] = {0};
    if (!is_gles) glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if ((!is_gles && (profile & GL_CONTEXT_CORE_PROFILE_BIT) == 0) || framebuffer == 0 ||
        samples != 0 || viewport[2] <= 0 || viewport[3] <= 0 ||
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        boxrig_gtk_gl_area_record_error(state, BOXRIG_GTK_RENDER_REASON_CAPABILITY,
            BOXRIG_UI_UNAVAILABLE, "GTK requires a complete, single-sample GL framebuffer");
        return state->status;
    }
    const GLenum channels[] = {GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
        GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,
        GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE};
    GLint alpha_bits = 0;
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        GLint bits = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, channels[i], &bits);
        if (i == 3) alpha_bits = bits;
        if (bits != 8 && !(i == 3 && bits == 0)) {
            boxrig_gtk_gl_area_record_error(state, BOXRIG_GTK_RENDER_REASON_CAPABILITY,
                BOXRIG_UI_UNAVAILABLE, "GTK GL color storage must be RGB8 or RGBA8");
            return state->status;
        }
    }
    GLint encoding = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
    if (glGetError() != GL_NO_ERROR || (encoding != GL_LINEAR && encoding != GL_SRGB)) {
        boxrig_gtk_gl_area_record_error(state, BOXRIG_GTK_RENDER_REASON_CAPABILITY,
            BOXRIG_UI_UNAVAILABLE, "GTK GL framebuffer encoding could not be established");
        return state->status;
    }
    *runtime = (boxrig_gl_runtime_config){NULL, boxrig_gtk_gl_area_resolve,
        area, current_context, context_generation, target_generation};
    *destination = (boxrig_gtk_gl_destination){(uint32_t)framebuffer,
        (uint32_t)viewport[2], (uint32_t)viewport[3], target_generation,
        encoding == GL_SRGB, (unsigned int)alpha_bits, 0};
    state->context_generation = context_generation;
    state->target_generation = target_generation;
    state->is_gles = is_gles;
    return BOXRIG_UI_OK;
}
