/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "gl_surface.h"
#include "gl_area.h"

#include <epoxy/gl.h>
#include <limits.h>
#include <stdlib.h>

struct boxrig_gtk_gl_surface {
    boxrig_ogl_renderer *renderer;
    boxrig_renderer_lifecycle *lifecycle;
    boxrig_renderer_frame_token token;
    boxrig_gl_runtime_config runtime;
    boxrig_gl_current_context owner;
    boxrig_gl_target target;
    GLuint texture;
    bool srgb_write_control;
    bool pending;
    bool complete;
};

static bool current(const boxrig_gtk_gl_surface *surface)
{
    boxrig_gl_current_context actual;
    return surface->runtime.query_current_context(
        surface->runtime.current_context_query_context, &actual) &&
        actual.identity == surface->owner.identity &&
        actual.thread_identity == surface->owner.thread_identity;
}

static boxrig_ui_status failure(boxrig_gtk_render_state *state,
    boxrig_gtk_render_reason reason, const char *message, GLenum error)
{
    boxrig_ui_status status = error == GL_OUT_OF_MEMORY
        ? BOXRIG_UI_OUT_OF_MEMORY : BOXRIG_UI_UNAVAILABLE;
    boxrig_gtk_gl_area_record_error(state, reason, status, message);
    state->gl_code = error;
    return status;
}

void boxrig_gtk_gl_surface_destroy(boxrig_gtk_gl_surface **surface_pointer)
{
    if (surface_pointer == NULL || *surface_pointer == NULL) return;
    boxrig_gtk_gl_surface *surface = *surface_pointer;
    bool can_delete = current(surface);
    /* The renderer contract releases CPU state after context loss. GTK owns
     * the abandoned context/share group; never delete its names in a new one. */
    boxrig_renderer_lifecycle_destroy(surface->lifecycle);
    (void)boxrig_ogl_renderer_destroy(&surface->renderer,
        surface->runtime.context_generation, NULL);
    if (can_delete) {
        glDeleteFramebuffers(1, &surface->target.draw_framebuffer);
        glDeleteTextures(1, &surface->texture);
    }
    free(surface);
    *surface_pointer = NULL;
}

boxrig_ui_status boxrig_gtk_gl_surface_create(
    const boxrig_ogl_renderer_config *config, boxrig_gtk_gl_surface **surface_out,
    boxrig_gtk_render_state *state)
{
    if (config == NULL || surface_out == NULL || state == NULL ||
        config->runtime.query_current_context == NULL ||
        config->runtime.resolve == NULL || config->initial_target.width == 0 ||
        config->initial_target.height == 0 || config->initial_target.width > INT_MAX ||
        config->initial_target.height > INT_MAX || config->initial_target.samples != 0 ||
        config->initial_target.generation == 0 || config->runtime.context_generation == 0 ||
        config->runtime.target_generation != config->initial_target.generation ||
        (config->initial_target.format != BOXRIG_GL_TARGET_RGBA8 &&
         config->initial_target.format != BOXRIG_GL_TARGET_SRGB8_ALPHA8)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *surface_out = NULL;
    boxrig_gtk_gl_surface *surface = calloc(1, sizeof(*surface));
    if (surface == NULL) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_RESOURCE,
            "Could not allocate a retained GL surface", GL_OUT_OF_MEMORY);
    }
    surface->runtime = config->runtime;
    if (!surface->runtime.query_current_context(
            surface->runtime.current_context_query_context, &surface->owner)) {
        free(surface);
        return failure(state, BOXRIG_GTK_RENDER_REASON_CONTEXT,
            "GTK GL context is not current", GL_NO_ERROR);
    }
    surface->target = config->initial_target;
    surface->srgb_write_control = epoxy_is_desktop_gl() ||
        epoxy_has_gl_extension("GL_EXT_sRGB_write_control");
    surface->target.draw_framebuffer = 0;
    GLint previous_texture = 0, previous_read = 0, previous_draw = 0, previous_unpack = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previous_unpack);
    glGenTextures(1, &surface->texture);
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexImage2D(GL_TEXTURE_2D, 0,
        surface->target.format == BOXRIG_GL_TARGET_SRGB8_ALPHA8 ? GL_SRGB8_ALPHA8 : GL_RGBA8,
        (GLsizei)surface->target.width, (GLsizei)surface->target.height,
        0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &surface->target.draw_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, surface->target.draw_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface->texture, 0);
    GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum error = glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)previous_unpack);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    if (error != GL_NO_ERROR || framebuffer_status != GL_FRAMEBUFFER_COMPLETE ||
        surface->texture == 0 || surface->target.draw_framebuffer == 0) {
        boxrig_gtk_gl_surface_destroy(&surface);
        return failure(state, BOXRIG_GTK_RENDER_REASON_RESOURCE,
            "Could not create the retained GL framebuffer", error);
    }
    boxrig_ogl_renderer_config retained = *config;
    retained.initial_target = surface->target;
    boxrig_gl_diagnostic diagnostic;
    if (boxrig_ogl_renderer_create(&retained, &surface->renderer, &diagnostic) != BOXRIG_GL_OK) {
        boxrig_gtk_gl_area_record_diagnostic(state, &diagnostic);
        boxrig_gtk_gl_surface_destroy(&surface);
        return state->status;
    }
    boxrig_renderer_target target = {0};
    target.generation = surface->target.generation;
    target.width = surface->target.width;
    target.height = surface->target.height;
    target.scale_numerator = 1;
    target.scale_denominator = 1;
    target.premultiplied_alpha = surface->target.alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED;
    if (boxrig_ogl_renderer_lifecycle_create(surface->renderer, &target,
            &surface->lifecycle) != BOXRIG_RENDERER_OK) {
        boxrig_gtk_gl_surface_destroy(&surface);
        return failure(state, BOXRIG_GTK_RENDER_REASON_RESOURCE,
            "Could not create the retained GL frame lifecycle", GL_NO_ERROR);
    }
    *surface_out = surface;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk_gl_surface_resize(boxrig_gtk_gl_surface *surface,
    const boxrig_gl_target *destination, boxrig_gtk_render_state *state)
{
    if (surface == NULL || destination == NULL || state == NULL ||
        destination->draw_framebuffer == 0 || destination->width == 0 ||
        destination->height == 0 || destination->width > INT_MAX ||
        destination->height > INT_MAX || destination->samples != 0 ||
        destination->generation <= surface->target.generation ||
        destination->format != surface->target.format ||
        destination->alpha_mode != surface->target.alpha_mode ||
        destination->origin != surface->target.origin || surface->pending) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!current(surface)) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_CONTEXT_LOST,
            "Retained GL surface context or thread changed", GL_NO_ERROR);
    }

    GLint previous_texture = 0, previous_read = 0, previous_draw = 0, previous_unpack = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previous_unpack);
    GLuint texture = 0, framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexImage2D(GL_TEXTURE_2D, 0,
        destination->format == BOXRIG_GL_TARGET_SRGB8_ALPHA8 ? GL_SRGB8_ALPHA8 : GL_RGBA8,
        (GLsizei)destination->width, (GLsizei)destination->height,
        0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum error = glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)previous_unpack);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    if (error != GL_NO_ERROR || framebuffer_status != GL_FRAMEBUFFER_COMPLETE ||
        texture == 0 || framebuffer == 0) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        return failure(state, BOXRIG_GTK_RENDER_REASON_RESOURCE,
            "Could not resize the retained GL framebuffer", error);
    }

    boxrig_gl_target target = *destination;
    target.draw_framebuffer = framebuffer;
    boxrig_gl_diagnostic diagnostic;
    if (boxrig_ogl_renderer_set_target(surface->renderer,
            surface->runtime.context_generation, &target, &diagnostic) != BOXRIG_GL_OK) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        boxrig_gtk_gl_area_record_diagnostic(state, &diagnostic);
        return state->status;
    }
    boxrig_renderer_target lifecycle_target = {0};
    lifecycle_target.generation = target.generation;
    lifecycle_target.width = target.width;
    lifecycle_target.height = target.height;
    lifecycle_target.scale_numerator = 1;
    lifecycle_target.scale_denominator = 1;
    lifecycle_target.premultiplied_alpha = target.alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED;
    if (boxrig_renderer_lifecycle_prepare_target(surface->lifecycle,
            &lifecycle_target) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_commit_target(surface->lifecycle) != BOXRIG_RENDERER_OK) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        return failure(state, BOXRIG_GTK_RENDER_REASON_RESOURCE,
            "Could not retarget the retained GL frame lifecycle", GL_NO_ERROR);
    }

    GLuint old_framebuffer = surface->target.draw_framebuffer;
    GLuint old_texture = surface->texture;
    surface->target = target;
    surface->texture = texture;
    surface->complete = false;
    glDeleteFramebuffers(1, &old_framebuffer);
    glDeleteTextures(1, &old_texture);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition,
    boxrig_gtk_render_state *state)
{
    if (surface == NULL || view == NULL || state == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (!current(surface)) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_CONTEXT_LOST,
            "Retained GL surface context or thread changed", GL_NO_ERROR);
    }
    if (surface->pending) return BOXRIG_UI_BUSY;
    boxrig_ui_transparency_state transparency;
    if (boxrig_ui_view_get_transparency(view, &transparency) != BOXRIG_UI_OK ||
        boxrig_ogl_renderer_apply_transparency_state(surface->renderer, &transparency) != 0) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_DRAW,
            "Could not apply the terminal surface alpha policy", GL_NO_ERROR);
    }
    surface->complete = false;
    if (boxrig_renderer_lifecycle_prepare_frame(surface->lifecycle, view, composition,
            surface->target.generation, &surface->token) != BOXRIG_RENDERER_OK) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_DRAW,
            "Retained GL frame preparation failed", GL_NO_ERROR);
    }
    surface->pending = true;
    return BOXRIG_UI_OK;
}

static void abort_frame(boxrig_gtk_gl_surface *surface)
{
    if (surface->pending) {
        (void)boxrig_renderer_lifecycle_abort_frame(surface->lifecycle, &surface->token);
        surface->pending = false;
    }
    surface->complete = false;
}

boxrig_ui_status boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gl_target *destination, boxrig_gtk_render_state *state)
{
    if (surface == NULL || destination == NULL || state == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (!current(surface)) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_CONTEXT_LOST,
            "Retained GL surface context or thread changed", GL_NO_ERROR);
    }
    if (!surface->pending && !surface->complete) {
        return failure(state, BOXRIG_GTK_RENDER_REASON_DRAW,
            "No complete GL image is available for presentation", GL_NO_ERROR);
    }
    if (destination->draw_framebuffer == 0 || destination->width != surface->target.width ||
        destination->height != surface->target.height || destination->samples != 0 ||
        destination->generation < surface->target.generation ||
        destination->alpha_mode != surface->target.alpha_mode ||
        destination->format != surface->target.format) {
        abort_frame(surface);
        return failure(state, BOXRIG_GTK_RENDER_REASON_CAPABILITY,
            "GTK and retained GL target properties do not match", GL_NO_ERROR);
    }
    GLint previous_read = 0, previous_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb = surface->srgb_write_control ? glIsEnabled(GL_FRAMEBUFFER_SRGB) : GL_FALSE;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, surface->target.draw_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination->draw_framebuffer);
    bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glDisable(GL_SCISSOR_TEST);
        /* GLES without EXT_sRGB_write_control always converts sRGB targets. */
        if (surface->srgb_write_control) {
            if (destination->format == BOXRIG_GL_TARGET_SRGB8_ALPHA8) glEnable(GL_FRAMEBUFFER_SRGB);
            else glDisable(GL_FRAMEBUFFER_SRGB);
        }
        glBlitFramebuffer(0, 0, (GLint)surface->target.width, (GLint)surface->target.height,
            0, 0, (GLint)destination->width, (GLint)destination->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    GLenum error = glGetError();
    if (scissor) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    if (surface->srgb_write_control) {
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
        else glDisable(GL_FRAMEBUFFER_SRGB);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);
    if (!complete || error != GL_NO_ERROR) {
        abort_frame(surface);
        return failure(state, BOXRIG_GTK_RENDER_REASON_DRAW,
            "Could not transfer the complete image to GTK", error);
    }
    if (surface->pending && boxrig_renderer_lifecycle_accept_frame(surface->lifecycle,
            &surface->token) != BOXRIG_RENDERER_OK) {
        abort_frame(surface);
        return failure(state, BOXRIG_GTK_RENDER_REASON_DRAW,
            "Could not finish the transferred GL frame", GL_NO_ERROR);
    }
    surface->pending = false;
    surface->complete = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk_gl_surface_get_target(
    const boxrig_gtk_gl_surface *surface, boxrig_gl_target *target_out)
{
    if (surface == NULL || target_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *target_out = surface->target;
    return BOXRIG_UI_OK;
}

unsigned int boxrig_gtk_gl_surface_cell_height(const boxrig_gtk_gl_surface *surface)
{
    return surface == NULL ? 0 : boxrig_ogl_renderer_cell_height(surface->renderer);
}

int boxrig_gtk_gl_surface_get_statistics(const boxrig_gtk_gl_surface *surface,
    boxrig_ogl_render_statistics *statistics_out)
{
    return surface == NULL ? -1 : boxrig_ogl_renderer_get_render_statistics(surface->renderer, statistics_out);
}

void boxrig_gtk_gl_surface_fail_next(boxrig_gtk_gl_surface *surface,
    boxrig_ogl_failure_point point)
{
    if (surface != NULL) boxrig_ogl_renderer_fail_next(surface->renderer, point);
}
