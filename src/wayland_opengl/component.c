/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define GL_GLEXT_PROTOTYPES 1

#include "boxrig/wayland/opengl_component.h"

#include "opengl/renderer.h"
#include "ui/renderer_lifecycle.h"
#include "wayland/component_private.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <wayland-egl.h>

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_wayland_egl_display_entry {
    struct wl_display *display;
    EGLDisplay egl_display;
    size_t reference_count;
    struct boxrig_wayland_egl_display_entry *next;
} boxrig_wayland_egl_display_entry;

typedef struct boxrig_wayland_ogl_framebuffer {
    GLuint framebuffer;
    GLuint texture;
    unsigned int width;
    unsigned int height;
} boxrig_wayland_ogl_framebuffer;

typedef struct boxrig_wayland_ogl_font_state {
    char *family_list;
    char **resource_paths;
    boxrig_font_feature *features;
    boxrig_font_variation *variations;
    boxrig_font_specification specification;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_effective_metrics effective_metrics;
    boxrig_text_direction_policy direction_policy;
    size_t resource_path_count;
    bool valid;
} boxrig_wayland_ogl_font_state;

typedef struct boxrig_wayland_ogl_adapter {
    boxrig_wayland_terminal_component *component;
    struct wl_display *display;
    struct wl_surface *surface;
    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLSurface egl_surface;
    EGLContext egl_context;
    boxrig_ogl_renderer *renderer;
    boxrig_ogl_renderer *font_candidate_renderer;
    boxrig_wayland_ogl_font_state font;
    boxrig_wayland_ogl_font_state font_candidate;
    boxrig_wayland_ogl_framebuffer committed;
    boxrig_wayland_ogl_framebuffer candidate;
    boxrig_renderer_lifecycle *candidate_lifecycle;
    boxrig_renderer_frame_token candidate_token;
    boxrig_ui_transparency_state transparency;
    uint64_t context_generation;
    uint64_t target_generation;
    uint64_t accepted_swap_count;
    uint64_t full_redraw_count;
    int swap_interval;
    int alpha_size;
    bool display_acquired;
    bool candidate_active;
    bool candidate_frame_active;
    bool target_lost;
    bool transparency_valid;
    bool scrollbar_enabled;
    unsigned int scrollbar_width;
    bool scrollbar_hovered;
    bool scrollbar_dragging;
} boxrig_wayland_ogl_adapter;

static pthread_mutex_t display_entries_mutex = PTHREAD_MUTEX_INITIALIZER;
static boxrig_wayland_egl_display_entry *display_entries;

static void destroy_font_state(boxrig_wayland_ogl_font_state *state)
{
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0; index < state->resource_path_count; index++) {
        free(state->resource_paths[index]);
    }
    free(state->resource_paths);
    free(state->features);
    free(state->variations);
    free(state->family_list);
    memset(state, 0, sizeof(*state));
}

static bool copy_font_state(
    const boxrig_wayland_component_font_presentation *source,
    boxrig_wayland_ogl_font_state *destination)
{
    boxrig_wayland_ogl_font_state copy;
    size_t index;

    if (source == NULL || source->family_list == NULL ||
        source->family_list_length == 0 || source->specification == NULL ||
        source->rendering_policy == NULL ||
        source->effective_metrics == NULL ||
        (source->resource_paths == NULL && source->resource_path_count != 0)) {
        return false;
    }
    memset(&copy, 0, sizeof(copy));
    copy.family_list = malloc(source->family_list_length + 1);
    if (copy.family_list == NULL) {
        return false;
    }
    memcpy(copy.family_list, source->family_list, source->family_list_length);
    copy.family_list[source->family_list_length] = '\0';
    copy.specification = *source->specification;
    copy.specification.families = NULL;
    copy.specification.family_count = 0;
    copy.specification.features = NULL;
    copy.specification.variations = NULL;
    if (source->specification->feature_count != 0) {
        if (source->specification->feature_count >
            SIZE_MAX / sizeof(*copy.features)) {
            destroy_font_state(&copy);
            return false;
        }
        copy.features = malloc(
            source->specification->feature_count * sizeof(*copy.features));
        if (copy.features == NULL) {
            destroy_font_state(&copy);
            return false;
        }
        memcpy(
            copy.features,
            source->specification->features,
            source->specification->feature_count * sizeof(*copy.features));
        copy.specification.features = copy.features;
    }
    if (source->specification->variation_count != 0) {
        if (source->specification->variation_count >
            SIZE_MAX / sizeof(*copy.variations)) {
            destroy_font_state(&copy);
            return false;
        }
        copy.variations = malloc(
            source->specification->variation_count *
            sizeof(*copy.variations));
        if (copy.variations == NULL) {
            destroy_font_state(&copy);
            return false;
        }
        memcpy(
            copy.variations,
            source->specification->variations,
            source->specification->variation_count *
                sizeof(*copy.variations));
        copy.specification.variations = copy.variations;
    }
    if (source->resource_path_count != 0) {
        if (source->resource_path_count >
            SIZE_MAX / sizeof(*copy.resource_paths)) {
            destroy_font_state(&copy);
            return false;
        }
        copy.resource_paths = calloc(
            source->resource_path_count,
            sizeof(*copy.resource_paths));
        if (copy.resource_paths == NULL) {
            destroy_font_state(&copy);
            return false;
        }
        copy.resource_path_count = source->resource_path_count;
        for (index = 0; index < source->resource_path_count; index++) {
            size_t length;

            if (source->resource_paths[index] == NULL) {
                destroy_font_state(&copy);
                return false;
            }
            length = strlen(source->resource_paths[index]);
            copy.resource_paths[index] = malloc(length + 1);
            if (copy.resource_paths[index] == NULL) {
                destroy_font_state(&copy);
                return false;
            }
            memcpy(
                copy.resource_paths[index],
                source->resource_paths[index],
                length + 1);
        }
    }
    copy.rendering_policy = *source->rendering_policy;
    copy.effective_metrics = *source->effective_metrics;
    copy.direction_policy = source->direction_policy;
    copy.valid = true;
    *destination = copy;
    return true;
}

static bool has_extension(const char *extensions, const char *name)
{
    const char *current;
    size_t length;

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

static EGLDisplay platform_display(struct wl_display *display)
{
    const char *extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    EGLDisplay egl_display = eglGetPlatformDisplay(
        EGL_PLATFORM_WAYLAND_KHR,
        display,
        NULL);

    if (egl_display == EGL_NO_DISPLAY &&
        has_extension(extensions, "EGL_EXT_platform_wayland")) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
                "eglGetPlatformDisplayEXT");

        if (get_platform_display != NULL) {
            egl_display = get_platform_display(
                EGL_PLATFORM_WAYLAND_EXT,
                display,
                NULL);
        }
    }
    return egl_display;
}

/* EGL initialization is display-wide, so terminate only after the final
 * adapter has released its context and window surface. */
static EGLDisplay acquire_display(struct wl_display *display)
{
    boxrig_wayland_egl_display_entry *entry;
    EGLDisplay egl_display;
    EGLint major;
    EGLint minor;

    if (pthread_mutex_lock(&display_entries_mutex) != 0) {
        return EGL_NO_DISPLAY;
    }
    for (entry = display_entries; entry != NULL; entry = entry->next) {
        if (entry->display == display) {
            entry->reference_count++;
            egl_display = entry->egl_display;
            (void)pthread_mutex_unlock(&display_entries_mutex);
            return egl_display;
        }
    }
    egl_display = platform_display(display);
    if (egl_display == EGL_NO_DISPLAY ||
        eglInitialize(egl_display, &major, &minor) != EGL_TRUE || major < 1 ||
        (major == 1 && minor < 5)) {
        if (egl_display != EGL_NO_DISPLAY) {
            (void)eglTerminate(egl_display);
        }
        (void)pthread_mutex_unlock(&display_entries_mutex);
        return EGL_NO_DISPLAY;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        (void)eglTerminate(egl_display);
        (void)pthread_mutex_unlock(&display_entries_mutex);
        return EGL_NO_DISPLAY;
    }
    entry->display = display;
    entry->egl_display = egl_display;
    entry->reference_count = 1;
    entry->next = display_entries;
    display_entries = entry;
    (void)pthread_mutex_unlock(&display_entries_mutex);
    return egl_display;
}

static void release_display(
    struct wl_display *display,
    EGLDisplay egl_display)
{
    boxrig_wayland_egl_display_entry **link;

    if (pthread_mutex_lock(&display_entries_mutex) != 0) {
        return;
    }
    for (link = &display_entries; *link != NULL; link = &(*link)->next) {
        boxrig_wayland_egl_display_entry *entry = *link;

        if (entry->display != display || entry->egl_display != egl_display) {
            continue;
        }
        if (--entry->reference_count == 0) {
            *link = entry->next;
            (void)eglTerminate(entry->egl_display);
            free(entry);
        }
        break;
    }
    (void)pthread_mutex_unlock(&display_entries_mutex);
}

static EGLConfig choose_config(EGLDisplay display, int alpha_size)
{
    EGLConfig *configs = NULL;
    EGLConfig selected = NULL;
    EGLint count = 0;
    EGLint index;

    if (eglGetConfigs(display, NULL, 0, &count) != EGL_TRUE || count <= 0) {
        return NULL;
    }
    configs = calloc((size_t)count, sizeof(*configs));
    if (configs == NULL ||
        eglGetConfigs(display, configs, count, &count) != EGL_TRUE) {
        free(configs);
        return NULL;
    }
    for (index = 0; index < count; index++) {
        EGLint surface_type;
        EGLint renderable_type;
        EGLint color_buffer_type;
        EGLint red;
        EGLint green;
        EGLint blue;
        EGLint alpha;
        EGLint samples;

        if (eglGetConfigAttrib(
                display,
                configs[index],
                EGL_SURFACE_TYPE,
                &surface_type) != EGL_TRUE ||
            eglGetConfigAttrib(
                display,
                configs[index],
                EGL_RENDERABLE_TYPE,
                &renderable_type) != EGL_TRUE ||
            eglGetConfigAttrib(
                display,
                configs[index],
                EGL_COLOR_BUFFER_TYPE,
                &color_buffer_type) != EGL_TRUE ||
            eglGetConfigAttrib(display, configs[index], EGL_RED_SIZE, &red) !=
                EGL_TRUE ||
            eglGetConfigAttrib(
                display,
                configs[index],
                EGL_GREEN_SIZE,
                &green) != EGL_TRUE ||
            eglGetConfigAttrib(display, configs[index], EGL_BLUE_SIZE, &blue) !=
                EGL_TRUE ||
            eglGetConfigAttrib(
                display,
                configs[index],
                EGL_ALPHA_SIZE,
                &alpha) != EGL_TRUE ||
            eglGetConfigAttrib(display, configs[index], EGL_SAMPLES, &samples) !=
                EGL_TRUE) {
            continue;
        }
        if ((surface_type & EGL_WINDOW_BIT) != 0 &&
            (renderable_type & EGL_OPENGL_BIT) != 0 &&
            color_buffer_type == EGL_RGB_BUFFER && red == 8 && green == 8 &&
            blue == 8 && alpha == alpha_size && samples == 0) {
            selected = configs[index];
            break;
        }
    }
    free(configs);
    return selected;
}

static bool make_current(boxrig_wayland_ogl_adapter *adapter)
{
    return adapter != NULL && adapter->egl_display != EGL_NO_DISPLAY &&
           adapter->egl_surface != EGL_NO_SURFACE &&
           adapter->egl_context != EGL_NO_CONTEXT &&
           eglMakeCurrent(
               adapter->egl_display,
               adapter->egl_surface,
               adapter->egl_surface,
               adapter->egl_context) == EGL_TRUE;
}

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
    boxrig_wayland_ogl_adapter *adapter = context;
    EGLContext current;

    if (adapter == NULL || current_out == NULL) {
        return false;
    }
    current = eglGetCurrentContext();
    current_out->identity = (uintptr_t)current;
    current_out->thread_identity = current_thread_identity();
    return current != EGL_NO_CONTEXT && current == adapter->egl_context;
}

static bool create_framebuffer(
    unsigned int width,
    unsigned int height,
    boxrig_wayland_ogl_framebuffer *framebuffer_out)
{
    boxrig_wayland_ogl_framebuffer framebuffer;
    GLint previous_draw;
    GLint previous_read;
    GLint previous_texture;

    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX ||
        framebuffer_out == NULL) {
        return false;
    }
    memset(&framebuffer, 0, sizeof(framebuffer));
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGenTextures(1, &framebuffer.texture);
    glBindTexture(GL_TEXTURE_2D, framebuffer.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        (GLsizei)width,
        (GLsizei)height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        NULL);
    glGenFramebuffers(1, &framebuffer.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        framebuffer.texture,
        0);
    framebuffer.width = width;
    framebuffer.height = height;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        glDeleteFramebuffers(1, &framebuffer.framebuffer);
        glDeleteTextures(1, &framebuffer.texture);
        memset(&framebuffer, 0, sizeof(framebuffer));
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    *framebuffer_out = framebuffer;
    return framebuffer.framebuffer != 0 && framebuffer.texture != 0;
}

static void destroy_framebuffer(boxrig_wayland_ogl_framebuffer *framebuffer)
{
    if (framebuffer == NULL) {
        return;
    }
    if (framebuffer->framebuffer != 0) {
        glDeleteFramebuffers(1, &framebuffer->framebuffer);
    }
    if (framebuffer->texture != 0) {
        glDeleteTextures(1, &framebuffer->texture);
    }
    memset(framebuffer, 0, sizeof(*framebuffer));
}

static boxrig_gl_target gl_target(
    const boxrig_wayland_ogl_adapter *adapter,
    const boxrig_wayland_ogl_framebuffer *framebuffer,
    uint64_t generation)
{
    boxrig_gl_target target;

    memset(&target, 0, sizeof(target));
    target.draw_framebuffer = framebuffer->framebuffer;
    target.width = framebuffer->width;
    target.height = framebuffer->height;
    target.generation = generation;
    target.format = BOXRIG_GL_TARGET_RGBA8;
    target.alpha_mode = adapter->alpha_size == 8
                            ? BOXRIG_GL_ALPHA_PREMULTIPLIED
                            : BOXRIG_GL_ALPHA_OPAQUE;
    target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    return target;
}

static boxrig_renderer_target renderer_target(
    const boxrig_wayland_ogl_adapter *adapter,
    const boxrig_wayland_ogl_framebuffer *framebuffer,
    uint64_t generation)
{
    boxrig_renderer_target target;

    memset(&target, 0, sizeof(target));
    target.generation = generation;
    target.width = framebuffer->width;
    target.height = framebuffer->height;
    target.scale_numerator = 1;
    target.scale_denominator = 1;
    target.premultiplied_alpha = adapter->alpha_size == 8;
    if (target.premultiplied_alpha) {
        target.transparency_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    return target;
}

static bool create_renderer_instance(
    boxrig_wayland_ogl_adapter *adapter,
    const boxrig_wayland_ogl_framebuffer *framebuffer,
    const boxrig_wayland_ogl_font_state *font,
    boxrig_ogl_renderer **renderer_out)
{
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    boxrig_gl_diagnostic diagnostic;
    boxrig_gl_target target = gl_target(
        adapter,
        framebuffer,
        adapter->target_generation);

    config.runtime.resolver_context = adapter;
    config.runtime.resolve = resolve_gl_function;
    config.runtime.current_context_query_context = adapter;
    config.runtime.query_current_context = query_current_context;
    config.runtime.context_generation = adapter->context_generation;
    config.runtime.target_generation = target.generation;
    config.initial_target = target;
    if (font->valid) {
        config.font_family = font->family_list;
        config.font_family_length = strlen(font->family_list);
        config.font_files = (const char *const *)font->resource_paths;
        config.font_file_count = font->resource_path_count;
        config.font_size_dip = font->specification.size_dip;
        config.font_weight = font->specification.weight;
        config.font_style = font->specification.style;
        config.font_stretch = font->specification.stretch;
        config.font_fallback = font->specification.fallback;
        config.font_features = font->specification.features;
        config.font_feature_count = font->specification.feature_count;
        config.font_variations = font->specification.variations;
        config.font_variation_count = font->specification.variation_count;
        config.effective_metrics = &font->effective_metrics;
        config.rendering_policy = font->rendering_policy;
        config.direction_policy = font->direction_policy;
    }
    if (boxrig_ogl_renderer_create(
            &config,
            renderer_out,
            &diagnostic) != BOXRIG_GL_OK ||
        (adapter->transparency_valid &&
         boxrig_ogl_renderer_apply_transparency_state(
             *renderer_out,
             &adapter->transparency) == -1) ||
        boxrig_ogl_renderer_set_composited_scrollbar(*renderer_out,
            adapter->scrollbar_enabled, adapter->scrollbar_width) == -1 ||
        boxrig_ogl_renderer_set_composited_scrollbar_interaction(*renderer_out,
            adapter->scrollbar_hovered, adapter->scrollbar_dragging) == -1) {
        if (*renderer_out != NULL) {
            (void)boxrig_ogl_renderer_destroy(
                renderer_out,
                adapter->context_generation,
                NULL);
        }
        return false;
    }
    return true;
}

static bool create_renderer(boxrig_wayland_ogl_adapter *adapter)
{
    unsigned int width = adapter->committed.width == 0
                             ? 1
                             : adapter->committed.width;
    unsigned int height = adapter->committed.height == 0
                              ? 1
                              : adapter->committed.height;

    memset(&adapter->committed, 0, sizeof(adapter->committed));
    if (!create_framebuffer(width, height, &adapter->committed)) {
        return false;
    }
    adapter->target_generation++;
    if (adapter->target_generation == 0) {
        adapter->target_generation++;
    }
    adapter->full_redraw_count++;
    return create_renderer_instance(
        adapter,
        &adapter->committed,
        &adapter->font,
        &adapter->renderer);
}

static bool create_context_and_renderer(boxrig_wayland_ogl_adapter *adapter)
{
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION,
        3,
        EGL_CONTEXT_MINOR_VERSION,
        3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};
    EGLint render_buffer;

    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
        return false;
    }
    adapter->egl_surface = eglCreateWindowSurface(
        adapter->egl_display,
        adapter->egl_config,
        (EGLNativeWindowType)adapter->egl_window,
        NULL);
    adapter->egl_context = eglCreateContext(
        adapter->egl_display,
        adapter->egl_config,
        EGL_NO_CONTEXT,
        context_attributes);
    if (adapter->egl_surface == EGL_NO_SURFACE ||
        adapter->egl_context == EGL_NO_CONTEXT || !make_current(adapter) ||
        eglQuerySurface(
            adapter->egl_display,
            adapter->egl_surface,
            EGL_RENDER_BUFFER,
            &render_buffer) != EGL_TRUE ||
        render_buffer != EGL_BACK_BUFFER ||
        eglSwapInterval(adapter->egl_display, adapter->swap_interval) !=
            EGL_TRUE) {
        return false;
    }
    adapter->context_generation++;
    if (adapter->context_generation == 0) {
        adapter->context_generation++;
    }
    return create_renderer(adapter);
}

static void release_context_resources(boxrig_wayland_ogl_adapter *adapter)
{
    bool current = make_current(adapter);

    boxrig_renderer_lifecycle_destroy(adapter->candidate_lifecycle);
    adapter->candidate_lifecycle = NULL;
    adapter->candidate_frame_active = false;
    adapter->candidate_active = false;
    if (adapter->font_candidate_renderer != NULL) {
        (void)boxrig_ogl_renderer_destroy(
            &adapter->font_candidate_renderer,
            adapter->context_generation,
            NULL);
    }
    destroy_font_state(&adapter->font_candidate);
    if (adapter->renderer != NULL) {
        (void)boxrig_ogl_renderer_destroy(
            &adapter->renderer,
            adapter->context_generation,
            NULL);
    }
    if (current) {
        destroy_framebuffer(&adapter->candidate);
        destroy_framebuffer(&adapter->committed);
    } else {
        memset(&adapter->candidate, 0, sizeof(adapter->candidate));
        memset(&adapter->committed, 0, sizeof(adapter->committed));
    }
    if (adapter->egl_context != EGL_NO_CONTEXT) {
        (void)eglMakeCurrent(
            adapter->egl_display,
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            EGL_NO_CONTEXT);
        (void)eglDestroyContext(adapter->egl_display, adapter->egl_context);
        adapter->egl_context = EGL_NO_CONTEXT;
    }
    if (adapter->egl_surface != EGL_NO_SURFACE) {
        (void)eglDestroySurface(adapter->egl_display, adapter->egl_surface);
        adapter->egl_surface = EGL_NO_SURFACE;
    }
}

static bool recreate_context(boxrig_wayland_ogl_adapter *adapter)
{
    unsigned int width = adapter->committed.width;
    unsigned int height = adapter->committed.height;

    release_context_resources(adapter);
    adapter->committed.width = width;
    adapter->committed.height = height;
    adapter->target_lost = false;
    return create_context_and_renderer(adapter);
}

static bool ensure_current(boxrig_wayland_ogl_adapter *adapter)
{
    if (adapter->target_lost) {
        return recreate_context(adapter);
    }
    if (make_current(adapter)) {
        return true;
    }
    adapter->target_lost = true;
    return recreate_context(adapter);
}

static bool set_renderer_target(
    boxrig_wayland_ogl_adapter *adapter,
    const boxrig_wayland_ogl_framebuffer *framebuffer)
{
    boxrig_gl_target target;

    adapter->target_generation++;
    if (adapter->target_generation == 0) {
        adapter->target_generation++;
    }
    target = gl_target(adapter, framebuffer, adapter->target_generation);
    return boxrig_ogl_renderer_set_target(
               adapter->renderer,
               adapter->context_generation,
               &target,
               NULL) == BOXRIG_GL_OK;
}

static bool present_framebuffer(
    boxrig_wayland_ogl_adapter *adapter,
    const boxrig_wayland_ogl_framebuffer *framebuffer)
{
    if (!ensure_current(adapter) || framebuffer->width == 0 ||
        framebuffer->height == 0) {
        return false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(
        0,
        0,
        (GLint)framebuffer->width,
        (GLint)framebuffer->height,
        0,
        0,
        (GLint)framebuffer->width,
        (GLint)framebuffer->height,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST);
    /* Layout has resolved the preedit caret. Version 2 cursor rectangles
     * must be committed before EGL commits the corresponding surface buffer. */
    boxrig_wayland_input_update_cursor_rectangle(adapter->component);
    if (glGetError() != GL_NO_ERROR ||
        eglSwapBuffers(adapter->egl_display, adapter->egl_surface) != EGL_TRUE) {
        EGLint error = eglGetError();

        if (error == EGL_CONTEXT_LOST || error == EGL_BAD_CONTEXT ||
            error == EGL_BAD_SURFACE) {
            adapter->target_lost = true;
        }
        return false;
    }
    adapter->accepted_swap_count++;
    return true;
}

static int adapter_prepare_font(
    void *context,
    const boxrig_wayland_component_font_presentation *font)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL || adapter->font_candidate_renderer != NULL ||
        adapter->candidate_active || !ensure_current(adapter) ||
        !copy_font_state(font, &adapter->font_candidate) ||
        !create_renderer_instance(
            adapter,
            &adapter->committed,
            &adapter->font_candidate,
            &adapter->font_candidate_renderer)) {
        if (adapter != NULL) {
            if (adapter->font_candidate_renderer != NULL) {
                (void)boxrig_ogl_renderer_destroy(
                    &adapter->font_candidate_renderer,
                    adapter->context_generation,
                    NULL);
            }
            destroy_font_state(&adapter->font_candidate);
        }
        return -1;
    }
    return 0;
}

static int adapter_commit_font(void *context)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL || adapter->font_candidate_renderer == NULL) {
        return -1;
    }
    if (adapter->renderer != NULL) {
        (void)boxrig_ogl_renderer_destroy(
            &adapter->renderer,
            adapter->context_generation,
            NULL);
    }
    adapter->renderer = adapter->font_candidate_renderer;
    adapter->font_candidate_renderer = NULL;
    destroy_font_state(&adapter->font);
    adapter->font = adapter->font_candidate;
    memset(&adapter->font_candidate, 0, sizeof(adapter->font_candidate));
    return 0;
}

static void adapter_abort_font(void *context)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL) {
        return;
    }
    if (adapter->font_candidate_renderer != NULL && ensure_current(adapter)) {
        (void)boxrig_ogl_renderer_destroy(
            &adapter->font_candidate_renderer,
            adapter->context_generation,
            NULL);
    }
    adapter->font_candidate_renderer = NULL;
    destroy_font_state(&adapter->font_candidate);
}

static int adapter_set_text_direction(
    void *context,
    boxrig_text_direction_policy policy)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL || !ensure_current(adapter) ||
        boxrig_ogl_renderer_set_direction_policy(adapter->renderer, policy) ==
            -1) {
        return -1;
    }
    if (adapter->font.valid) {
        adapter->font.direction_policy = policy;
    }
    return 0;
}

static int adapter_apply_transparency_state(
    void *context,
    const boxrig_ui_transparency_state *state)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL || state == NULL || !ensure_current(adapter) ||
        boxrig_ogl_renderer_apply_transparency_state(
            adapter->renderer,
            state) == -1) {
        return -1;
    }
    adapter->transparency = *state;
    adapter->transparency_valid = true;
    return 0;
}

static int adapter_prepare_surface_candidate(
    void *context,
    unsigned int width,
    unsigned int height)
{
    boxrig_wayland_ogl_adapter *adapter = context;
    boxrig_renderer_target target;

    if (adapter == NULL || adapter->candidate_active || width == 0 ||
        height == 0 || !ensure_current(adapter) ||
        !create_framebuffer(width, height, &adapter->candidate) ||
        !set_renderer_target(adapter, &adapter->candidate)) {
        if (adapter != NULL) {
            destroy_framebuffer(&adapter->candidate);
        }
        return -1;
    }
    target = renderer_target(
        adapter,
        &adapter->candidate,
        adapter->target_generation);
    if (boxrig_ogl_renderer_lifecycle_create(
            adapter->renderer,
            &target,
            &adapter->candidate_lifecycle) != BOXRIG_RENDERER_OK) {
        (void)set_renderer_target(adapter, &adapter->committed);
        destroy_framebuffer(&adapter->candidate);
        return -1;
    }
    adapter->candidate_active = true;
    return 0;
}

static int adapter_commit_surface_candidate(void *context, bool present)
{
    boxrig_wayland_ogl_adapter *adapter = context;
    boxrig_wayland_ogl_framebuffer previous;

    if (adapter == NULL || !adapter->candidate_active ||
        !adapter->candidate_frame_active) {
        return -1;
    }
    wl_egl_window_resize(
        adapter->egl_window,
        (int)adapter->candidate.width,
        (int)adapter->candidate.height,
        0,
        0);
    if ((present && !present_framebuffer(adapter, &adapter->candidate)) ||
        boxrig_renderer_lifecycle_accept_frame(
            adapter->candidate_lifecycle,
            &adapter->candidate_token) != BOXRIG_RENDERER_OK) {
        wl_egl_window_resize(
            adapter->egl_window,
            (int)adapter->committed.width,
            (int)adapter->committed.height,
            0,
            0);
        return -1;
    }
    adapter->candidate_frame_active = false;
    boxrig_renderer_lifecycle_destroy(adapter->candidate_lifecycle);
    adapter->candidate_lifecycle = NULL;
    previous = adapter->committed;
    adapter->committed = adapter->candidate;
    memset(&adapter->candidate, 0, sizeof(adapter->candidate));
    adapter->candidate_active = false;
    destroy_framebuffer(&previous);
    adapter->full_redraw_count++;
    return 0;
}

static void adapter_abort_surface_candidate(void *context)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL || !adapter->candidate_active ||
        !ensure_current(adapter)) {
        return;
    }
    if (adapter->candidate_frame_active) {
        (void)boxrig_renderer_lifecycle_abort_frame(
            adapter->candidate_lifecycle,
            &adapter->candidate_token);
    }
    adapter->candidate_frame_active = false;
    boxrig_renderer_lifecycle_destroy(adapter->candidate_lifecycle);
    adapter->candidate_lifecycle = NULL;
    (void)set_renderer_target(adapter, &adapter->committed);
    destroy_framebuffer(&adapter->candidate);
    adapter->candidate_active = false;
}

static int adapter_draw_view_composition(
    void *context,
    boxrig_ui_view *view,
    boxrig_composition *composition,
    bool present)
{
    boxrig_wayland_ogl_adapter *adapter = context;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_frame_token token;
    boxrig_renderer_target target;
    int result = -1;

    if (adapter == NULL || view == NULL || !ensure_current(adapter)) {
        return -1;
    }
    if (adapter->candidate_active) {
        if (adapter->candidate_frame_active) {
            return 0;
        }
        if (boxrig_renderer_lifecycle_prepare_frame(
                adapter->candidate_lifecycle,
                view,
                composition,
                adapter->target_generation,
                &adapter->candidate_token) != BOXRIG_RENDERER_OK) {
            return -1;
        }
        adapter->candidate_frame_active = true;
        return 0;
    }
    target = renderer_target(
        adapter,
        &adapter->committed,
        adapter->target_generation);
    if (boxrig_ogl_renderer_lifecycle_create(
            adapter->renderer,
            &target,
            &lifecycle) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_prepare_frame(
            lifecycle,
            view,
            composition,
            adapter->target_generation,
            &token) != BOXRIG_RENDERER_OK) {
        goto cleanup;
    }
    if (present && !present_framebuffer(adapter, &adapter->committed)) {
        (void)boxrig_renderer_lifecycle_abort_frame(lifecycle, &token);
        goto cleanup;
    }
    if (boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) !=
        BOXRIG_RENDERER_OK) {
        goto cleanup;
    }
    result = 0;

cleanup:
    boxrig_renderer_lifecycle_destroy(lifecycle);
    return result;
}

static void adapter_destroy(void *context)
{
    boxrig_wayland_ogl_adapter *adapter = context;

    if (adapter == NULL) {
        return;
    }
    release_context_resources(adapter);
    destroy_font_state(&adapter->font);
    if (adapter->egl_window != NULL) {
        wl_egl_window_destroy(adapter->egl_window);
    }
    if (adapter->display_acquired) {
        release_display(adapter->display, adapter->egl_display);
    }
    free(adapter);
}

static int adapter_set_composited_scrollbar(void *context, bool enabled, unsigned int width)
{
    boxrig_wayland_ogl_adapter *adapter = context;
    if (!ensure_current(adapter) ||
        boxrig_ogl_renderer_set_composited_scrollbar(adapter->renderer, enabled, width) == -1)
        return -1;
    adapter->scrollbar_enabled = enabled;
    adapter->scrollbar_width = width;
    return 0;
}

static int adapter_set_composited_scrollbar_interaction(void *context, bool hovered, bool dragging)
{
    boxrig_wayland_ogl_adapter *adapter = context;
    if (!ensure_current(adapter) ||
        boxrig_ogl_renderer_set_composited_scrollbar_interaction(adapter->renderer, hovered, dragging) == -1)
        return -1;
    adapter->scrollbar_hovered = hovered;
    adapter->scrollbar_dragging = dragging;
    return 0;
}

static const boxrig_wayland_component_renderer_operations renderer_operations = {
    adapter_prepare_font,
    adapter_commit_font,
    adapter_abort_font,
    adapter_set_text_direction,
    adapter_apply_transparency_state,
    adapter_set_composited_scrollbar,
    adapter_set_composited_scrollbar_interaction,
    adapter_prepare_surface_candidate,
    adapter_commit_surface_candidate,
    adapter_abort_surface_candidate,
    adapter_draw_view_composition,
    adapter_destroy};

static boxrig_wayland_ogl_adapter *create_adapter(
    struct wl_display *display,
    struct wl_surface *surface,
    int alpha_size,
    int swap_interval)
{
    boxrig_wayland_ogl_adapter *adapter = calloc(1, sizeof(*adapter));

    if (adapter == NULL) {
        return NULL;
    }
    adapter->display = display;
    adapter->surface = surface;
    adapter->egl_display = EGL_NO_DISPLAY;
    adapter->egl_surface = EGL_NO_SURFACE;
    adapter->egl_context = EGL_NO_CONTEXT;
    adapter->alpha_size = alpha_size;
    adapter->swap_interval = swap_interval;
    adapter->scrollbar_width = 14;
    adapter->committed.width = 1;
    adapter->committed.height = 1;
    adapter->egl_display = acquire_display(display);
    adapter->display_acquired = adapter->egl_display != EGL_NO_DISPLAY;
    if (!adapter->display_acquired) {
        free(adapter);
        return NULL;
    }
    adapter->egl_config = choose_config(adapter->egl_display, alpha_size);
    adapter->egl_window = wl_egl_window_create(surface, 1, 1);
    if (adapter->egl_config == NULL || adapter->egl_window == NULL ||
        !create_context_and_renderer(adapter)) {
        adapter_destroy(adapter);
        return NULL;
    }
    return adapter;
}

boxrig_wayland_opengl_component_config
boxrig_wayland_opengl_component_config_default(void)
{
    boxrig_wayland_opengl_component_config config;

    config.swap_interval = 0;
    return config;
}

boxrig_ui_status boxrig_wayland_opengl_terminal_component_create(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_opengl_component_config opengl_config =
        boxrig_wayland_opengl_component_config_default();

    return boxrig_wayland_opengl_terminal_component_create_with_config(
        context,
        parent,
        model,
        config,
        &opengl_config,
        component_out);
}

boxrig_ui_status boxrig_wayland_opengl_terminal_component_create_with_config(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    const boxrig_wayland_opengl_component_config *opengl_config,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_terminal_component_config opaque_config;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_ogl_adapter *adapter;
    int alpha_size;
    boxrig_ui_status status;

    if (context == NULL || parent == NULL || model == NULL || config == NULL ||
        opengl_config == NULL || component_out == NULL ||
        (opengl_config->swap_interval != 0 &&
         opengl_config->swap_interval != 1)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (config->transparency.request == BOXRIG_UI_TRANSPARENCY_REQUIRED &&
        (config->transparency.requested_scopes &
         BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL) != 0) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    opaque_config = *config;
    opaque_config.transparency = boxrig_ui_transparency_config_default();
    status = boxrig_wayland_terminal_component_create(
        context,
        parent,
        model,
        &opaque_config,
        &component);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    /* Keep a runtime-capable alpha target even when the initial request is
     * opaque. Opaque presentation remains exact through the renderer state
     * and wl_surface opaque region, while a later transparency request cannot
     * replace an EGLConfig without replacing the component. */
    alpha_size = (config->transparency.requested_scopes &
                  BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0
                     ? 8
                     : 0;
    adapter = create_adapter(
        context->display,
        component->surface,
        alpha_size,
        opengl_config->swap_interval);
    if (adapter == NULL && alpha_size == 8 &&
        config->transparency.request == BOXRIG_UI_TRANSPARENCY_PREFERRED) {
        alpha_size = 0;
        adapter = create_adapter(
            context->display,
            component->surface,
            alpha_size,
            opengl_config->swap_interval);
    }
    if (adapter == NULL) {
        boxrig_wayland_terminal_component_destroy(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    adapter->component = component;
    status = boxrig_wayland_terminal_component_adopt_renderer(
        component,
        adapter,
        &renderer_operations,
        alpha_size == 8,
        &config->transparency);
    if (status != BOXRIG_UI_OK) {
        boxrig_wayland_terminal_component_destroy(component);
        return status;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_opengl_terminal_component_get_diagnostic(
    const boxrig_wayland_terminal_component *component,
    boxrig_wayland_opengl_component_diagnostic *diagnostic_out)
{
    const boxrig_wayland_ogl_adapter *adapter;

    if (component == NULL || diagnostic_out == NULL ||
        component->presentation_renderer_operations != &renderer_operations) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    adapter = component->presentation_renderer;
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    diagnostic_out->context_generation = adapter->context_generation;
    diagnostic_out->target_generation = adapter->target_generation;
    diagnostic_out->accepted_swap_count = adapter->accepted_swap_count;
    diagnostic_out->full_redraw_count = adapter->full_redraw_count;
    diagnostic_out->width = adapter->committed.width;
    diagnostic_out->height = adapter->committed.height;
    diagnostic_out->alpha_target = adapter->alpha_size == 8;
    diagnostic_out->target_lost = adapter->target_lost;
    diagnostic_out->partial_swap_enabled = false;
    return BOXRIG_UI_OK;
}
