/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define GL_GLEXT_PROTOTYPES 1

#include "boxrig/x11/opengl_component.h"

#include "opengl/renderer.h"
#include "ui/renderer_lifecycle.h"
#include "x11/component_private.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_x11_egl_display_entry {
    Display *x_display;
    int screen;
    EGLDisplay egl_display;
    size_t reference_count;
    struct boxrig_x11_egl_display_entry *next;
} boxrig_x11_egl_display_entry;

typedef struct boxrig_x11_ogl_framebuffer {
    GLuint framebuffer;
    GLuint texture;
    unsigned int width;
    unsigned int height;
} boxrig_x11_ogl_framebuffer;

typedef struct boxrig_x11_ogl_font_state {
    FcConfig *font_config;
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
} boxrig_x11_ogl_font_state;

typedef struct boxrig_x11_ogl_adapter {
    Display *display;
    Window window;
    int screen;
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLSurface egl_surface;
    EGLContext egl_context;
    boxrig_ogl_renderer *renderer;
    boxrig_ogl_renderer *font_candidate_renderer;
    boxrig_x11_ogl_font_state font;
    boxrig_x11_ogl_font_state font_candidate;
    boxrig_x11_ogl_framebuffer committed;
    boxrig_x11_ogl_framebuffer candidate;
    boxrig_renderer_lifecycle *candidate_lifecycle;
    boxrig_renderer_frame_token candidate_token;
    uint64_t context_generation;
    uint64_t target_generation;
    unsigned int target_width;
    unsigned int target_height;
    int viewport_x;
    int viewport_y;
    unsigned int viewport_width;
    unsigned int viewport_height;
    int swap_interval;
    int alpha_size;
    boxrig_ui_transparency_state transparency;
    unsigned int scrollbar_width;
    bool display_acquired;
    bool use_es;
    bool candidate_active;
    bool candidate_frame_active;
    bool expose_pending;
    bool presentation_pending;
    bool context_invalid;
    bool transparency_valid;
    bool scrollbar_enabled;
    bool scrollbar_hovered;
    bool scrollbar_dragging;
} boxrig_x11_ogl_adapter;

static pthread_mutex_t display_entries_mutex = PTHREAD_MUTEX_INITIALIZER;
static boxrig_x11_egl_display_entry *display_entries;

static void destroy_font_state(boxrig_x11_ogl_font_state *state)
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
    if (state->font_config != NULL) {
        FcConfigDestroy(state->font_config);
    }
    memset(state, 0, sizeof(*state));
}

static bool copy_font_state(
    const boxrig_x11_component_font_presentation *source,
    boxrig_x11_ogl_font_state *destination)
{
    boxrig_x11_ogl_font_state copy;
    size_t index;

    if (source == NULL || source->family_list == NULL ||
        source->family_list_length == 0 || source->specification == NULL ||
        source->rendering_policy == NULL ||
        source->effective_metrics == NULL || source->font_config == NULL ||
        (source->resource_paths == NULL && source->resource_path_count != 0)) {
        return false;
    }
    memset(&copy, 0, sizeof(copy));
    copy.font_config = FcConfigReference(source->font_config);
    if (copy.font_config == NULL) {
        return false;
    }
    copy.family_list = malloc(source->family_list_length + 1);
    if (copy.family_list == NULL) {
        destroy_font_state(&copy);
        return false;
    }
    memcpy(
        copy.family_list,
        source->family_list,
        source->family_list_length);
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

static bool valid_component_config(
    const boxrig_x11_terminal_component_config *config,
    bool borrowed)
{
    return config != NULL &&
           (config->attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY ||
            config->attachment == BOXRIG_UI_COMPONENT_PRESENTATION_ONLY) &&
           (config->scrollbar_presentation ==
                BOXRIG_UI_COMPONENT_SCROLLBAR_NONE ||
            config->scrollbar_presentation ==
                BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED ||
            (!borrowed &&
             config->scrollbar_presentation ==
                 BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD)) &&
           config->paste_chunk_size != 0 &&
           config->selection_transfer_chunk_size != 0 &&
           config->maximum_active_selection_transfers != 0 &&
           config->maximum_selection_transfer_bytes != 0;
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

static EGLDisplay platform_display(Display *display, int screen)
{
    const char *extensions =
        eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    const EGLAttrib attributes[] = {
        EGL_PLATFORM_X11_SCREEN_EXT,
        (EGLAttrib)screen,
        EGL_NONE};
    EGLDisplay egl_display = eglGetPlatformDisplay(
        EGL_PLATFORM_X11_EXT,
        display,
        attributes);

    if (egl_display == EGL_NO_DISPLAY &&
        has_extension(extensions, "EGL_EXT_platform_x11")) {
        const EGLint extension_attributes[] = {
            EGL_PLATFORM_X11_SCREEN_EXT,
            screen,
            EGL_NONE};
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
                "eglGetPlatformDisplayEXT");

        if (get_platform_display != NULL) {
            egl_display = get_platform_display(
                EGL_PLATFORM_X11_EXT,
                display,
                extension_attributes);
        }
    }
    return egl_display;
}

/* EGLDisplay initialization is display-wide. Keep it alive until the last
 * component releases its own context and surface. */
static EGLDisplay acquire_display(Display *display, int screen)
{
    boxrig_x11_egl_display_entry *entry;
    EGLDisplay egl_display;
    EGLint major;
    EGLint minor;

    if (pthread_mutex_lock(&display_entries_mutex) != 0) {
        return EGL_NO_DISPLAY;
    }
    for (entry = display_entries; entry != NULL; entry = entry->next) {
        if (entry->x_display == display && entry->screen == screen) {
            entry->reference_count++;
            egl_display = entry->egl_display;
            (void)pthread_mutex_unlock(&display_entries_mutex);
            return egl_display;
        }
    }
    egl_display = platform_display(display, screen);
    if (egl_display == EGL_NO_DISPLAY ||
        eglInitialize(egl_display, &major, &minor) != EGL_TRUE ||
        (major < 1 || (major == 1 && minor < 5))) {
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
    entry->x_display = display;
    entry->screen = screen;
    entry->egl_display = egl_display;
    entry->reference_count = 1;
    entry->next = display_entries;
    display_entries = entry;
    (void)pthread_mutex_unlock(&display_entries_mutex);
    return egl_display;
}

static void release_display(
    Display *display,
    int screen,
    EGLDisplay egl_display)
{
    boxrig_x11_egl_display_entry **link;

    if (pthread_mutex_lock(&display_entries_mutex) != 0) {
        return;
    }
    for (link = &display_entries; *link != NULL; link = &(*link)->next) {
        boxrig_x11_egl_display_entry *entry = *link;

        if (entry->x_display != display || entry->screen != screen ||
            entry->egl_display != egl_display) {
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

static bool compositor_active(Display *display, int screen)
{
    char selection_name[64];
    int event_base;
    int error_base;
    int length;
    Atom selection;

    if (!XCompositeQueryExtension(display, &event_base, &error_base)) {
        return false;
    }
    length = snprintf(
        selection_name,
        sizeof(selection_name),
        "_NET_WM_CM_S%d",
        screen);
    if (length <= 0 || (size_t)length >= sizeof(selection_name)) {
        return false;
    }
    selection = XInternAtom(display, selection_name, True);
    return selection != None && XGetSelectionOwner(display, selection) != None;
}

static EGLConfig choose_config(
    EGLDisplay egl_display,
    Display *display,
    int screen,
    int alpha_size,
    VisualID required_visual,
    bool use_es,
    EGLint *visual_id_out)
{
    EGLConfig *configs = NULL;
    EGLConfig selected = NULL;
    EGLint count = 0;
    EGLint index;

    if (eglGetConfigs(egl_display, NULL, 0, &count) != EGL_TRUE || count <= 0) {
        return NULL;
    }
    configs = calloc((size_t)count, sizeof(*configs));
    if (configs == NULL ||
        eglGetConfigs(egl_display, configs, count, &count) != EGL_TRUE) {
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
        EGLint visual_id;
        EGLint samples;
        XVisualInfo visual_template;
        XVisualInfo *visual_info;
        int visual_count;
        bool visual_compatible;

        if (eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_SURFACE_TYPE,
                &surface_type) != EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_RENDERABLE_TYPE,
                &renderable_type) != EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_COLOR_BUFFER_TYPE,
                &color_buffer_type) != EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_RED_SIZE,
                &red) !=
                EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_GREEN_SIZE,
                &green) != EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_BLUE_SIZE,
                &blue) !=
                EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_ALPHA_SIZE,
                &alpha) != EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_NATIVE_VISUAL_ID,
                &visual_id) != EGL_TRUE ||
            eglGetConfigAttrib(
                egl_display,
                configs[index],
                EGL_SAMPLES,
                &samples) !=
                EGL_TRUE) {
            continue;
        }
        if ((surface_type & EGL_WINDOW_BIT) == 0 ||
            (renderable_type & (use_es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT)) == 0 ||
            color_buffer_type != EGL_RGB_BUFFER || red != 8 || green != 8 ||
            blue != 8 || alpha != alpha_size || samples != 0 ||
            visual_id == 0 ||
            (required_visual != 0 &&
             (VisualID)(unsigned long)visual_id != required_visual)) {
            continue;
        }
        memset(&visual_template, 0, sizeof(visual_template));
        visual_template.visualid = (VisualID)(unsigned long)visual_id;
        visual_template.screen = screen;
        visual_info = XGetVisualInfo(
            display,
            VisualIDMask | VisualScreenMask,
            &visual_template,
            &visual_count);
        visual_compatible =
            visual_info != NULL && visual_count == 1 &&
            visual_info->depth == (alpha_size == 8 ? 32 : 24);
        if (visual_info != NULL) {
            XFree(visual_info);
        }
        if (!visual_compatible) {
            continue;
        }
        selected = configs[index];
        *visual_id_out = visual_id;
        break;
    }
    free(configs);
    return selected;
}

static EGLContext create_gl_context(
    EGLDisplay display, EGLConfig config, bool use_es)
{
    static const EGLint desktop_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};
    static const EGLint es_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE};

    if (eglBindAPI(use_es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) != EGL_TRUE)
        return EGL_NO_CONTEXT;
    return eglCreateContext(display, config, EGL_NO_CONTEXT,
        use_es ? es_attributes : desktop_attributes);
}

static EGLConfig choose_available_config(
    EGLDisplay egl_display, Display *display, int screen,
    int alpha_size, EGLint *visual_id_out)
{
    EGLConfig selected = NULL;
    EGLenum previous_api = eglQueryAPI();

    /* Probe context creation before the caller commits to an X11 Visual.
     * An advertised ES config can still be unusable with the active driver. */
    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        bool use_es = attempt == 0;
        EGLConfig config = choose_config(egl_display, display, screen,
            alpha_size, 0, use_es, visual_id_out);
        if (config == NULL) continue;
        EGLContext context = create_gl_context(egl_display, config, use_es);
        if (context == EGL_NO_CONTEXT) continue;
        (void)eglDestroyContext(egl_display, context);
        selected = config;
        break;
    }
    if (previous_api != EGL_NONE) (void)eglBindAPI(previous_api);
    return selected;
}

static bool make_current(boxrig_x11_ogl_adapter *adapter)
{
    if (adapter == NULL || adapter->egl_display == EGL_NO_DISPLAY ||
        adapter->egl_surface == EGL_NO_SURFACE ||
        adapter->egl_context == EGL_NO_CONTEXT) {
        return false;
    }
    /* EGL tracks the current context per client API. Another component may
     * have selected a different API since this adapter last rendered. */
    if (eglBindAPI(adapter->use_es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) != EGL_TRUE)
        return false;
    return eglMakeCurrent(
               adapter->egl_display,
               adapter->egl_surface,
               adapter->egl_surface,
               adapter->egl_context) == EGL_TRUE;
}

static boxrig_gl_function resolve_gl_function(void *context, const char *name)
{
    boxrig_x11_ogl_adapter *adapter = context;

    return adapter == NULL || name == NULL
               ? NULL
               : (boxrig_gl_function)eglGetProcAddress(name);
}

static bool query_current_context(
    void *context,
    boxrig_gl_current_context *current_out)
{
    boxrig_x11_ogl_adapter *adapter = context;
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
    boxrig_x11_ogl_framebuffer *framebuffer_out)
{
    boxrig_x11_ogl_framebuffer framebuffer;
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

static void destroy_framebuffer(boxrig_x11_ogl_framebuffer *framebuffer)
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
    const boxrig_x11_ogl_adapter *adapter,
    const boxrig_x11_ogl_framebuffer *framebuffer,
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
    const boxrig_x11_ogl_adapter *adapter,
    const boxrig_x11_ogl_framebuffer *framebuffer,
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
    boxrig_x11_ogl_adapter *adapter,
    const boxrig_x11_ogl_framebuffer *framebuffer,
    const boxrig_x11_ogl_font_state *font,
    boxrig_ogl_renderer **renderer_out)
{
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    boxrig_ogl_text_provider_config text_config;
    boxrig_ogl_text_provider *text_provider = NULL;
    boxrig_gl_diagnostic diagnostic;
    boxrig_gl_status status;
    boxrig_gl_target target;

    target = gl_target(
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
    memset(&text_config, 0, sizeof(text_config));
    text_config.font_family = config.font_family;
    text_config.font_family_length = config.font_family_length;
    text_config.font_files = config.font_files;
    text_config.font_file_count = config.font_file_count;
    text_config.font_size_dip = config.font_size_dip;
    text_config.font_weight = config.font_weight;
    text_config.font_style = config.font_style;
    text_config.font_stretch = config.font_stretch;
    text_config.font_fallback = config.font_fallback;
    text_config.font_features = config.font_features;
    text_config.font_feature_count = config.font_feature_count;
    text_config.font_variations = config.font_variations;
    text_config.font_variation_count = config.font_variation_count;
    text_config.rendering_policy = config.rendering_policy;
    text_config.direction_policy = config.direction_policy;
    text_config.native_font_config = font->font_config;
    if (boxrig_ogl_text_provider_create(&text_config, &text_provider) == -1) {
        return false;
    }
    status = boxrig_ogl_renderer_create_with_provider(
        &config,
        text_provider,
        renderer_out,
        &diagnostic);
    if (status != BOXRIG_GL_OK) {
        boxrig_ogl_text_provider_destroy(text_provider);
        return false;
    }
    if ((adapter->transparency_valid &&
         boxrig_ogl_renderer_apply_transparency_state(
             *renderer_out,
             &adapter->transparency) == -1) ||
        boxrig_ogl_renderer_set_composited_scrollbar(
            *renderer_out,
            adapter->scrollbar_enabled,
            adapter->scrollbar_width) == -1 ||
        boxrig_ogl_renderer_set_composited_scrollbar_interaction(
            *renderer_out,
            adapter->scrollbar_hovered,
            adapter->scrollbar_dragging) == -1) {
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

static bool create_renderer(boxrig_x11_ogl_adapter *adapter)
{
    unsigned int width =
        adapter->viewport_width == 0 ? 1 : adapter->viewport_width;
    unsigned int height =
        adapter->viewport_height == 0 ? 1 : adapter->viewport_height;

    if (!create_framebuffer(width, height, &adapter->committed)) {
        return false;
    }
    adapter->target_generation++;
    if (adapter->target_generation == 0) {
        adapter->target_generation++;
    }
    if (!adapter->font.valid) {
        return true;
    }
    return create_renderer_instance(
        adapter,
        &adapter->committed,
        &adapter->font,
        &adapter->renderer);
}

static bool create_context_and_renderer(boxrig_x11_ogl_adapter *adapter)
{
    EGLint render_buffer;
    EGLint color_space;

    adapter->egl_context = create_gl_context(
        adapter->egl_display, adapter->egl_config, adapter->use_es);
    if (adapter->egl_context == EGL_NO_CONTEXT) {
        return false;
    }
    adapter->egl_surface = eglCreateWindowSurface(
        adapter->egl_display,
        adapter->egl_config,
        (EGLNativeWindowType)adapter->window,
        NULL);
    if (adapter->egl_surface == EGL_NO_SURFACE ||
        !make_current(adapter) ||
        eglQuerySurface(
            adapter->egl_display,
            adapter->egl_surface,
            EGL_RENDER_BUFFER,
            &render_buffer) != EGL_TRUE ||
        render_buffer != EGL_BACK_BUFFER ||
        eglQuerySurface(
            adapter->egl_display,
            adapter->egl_surface,
            EGL_GL_COLORSPACE,
            &color_space) != EGL_TRUE ||
        color_space != EGL_GL_COLORSPACE_LINEAR ||
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

static void release_context_resources(boxrig_x11_ogl_adapter *adapter)
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
        (void)eglDestroyContext(
            adapter->egl_display,
            adapter->egl_context);
        adapter->egl_context = EGL_NO_CONTEXT;
    }
    if (adapter->egl_surface != EGL_NO_SURFACE) {
        (void)eglDestroySurface(
            adapter->egl_display,
            adapter->egl_surface);
        adapter->egl_surface = EGL_NO_SURFACE;
    }
}

static bool recreate_context(boxrig_x11_ogl_adapter *adapter)
{
    release_context_resources(adapter);
    adapter->context_invalid = false;
    return create_context_and_renderer(adapter);
}

static bool ensure_current(boxrig_x11_ogl_adapter *adapter)
{
    if (adapter->context_invalid) {
        return recreate_context(adapter);
    }
    if (make_current(adapter)) {
        return true;
    }
    adapter->context_invalid = true;
    return recreate_context(adapter);
}

static bool recreate_window_surface(boxrig_x11_ogl_adapter *adapter)
{
    EGLSurface replacement;
    EGLint render_buffer;
    EGLint color_space;

    if (!ensure_current(adapter) ||
        eglMakeCurrent(
            adapter->egl_display,
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            EGL_NO_CONTEXT) != EGL_TRUE) {
        return false;
    }
    (void)eglDestroySurface(adapter->egl_display, adapter->egl_surface);
    adapter->egl_surface = EGL_NO_SURFACE;
    replacement = eglCreateWindowSurface(
        adapter->egl_display,
        adapter->egl_config,
        (EGLNativeWindowType)adapter->window,
        NULL);
    if (replacement == EGL_NO_SURFACE) {
        adapter->context_invalid = true;
        return false;
    }
    adapter->egl_surface = replacement;
    if (!make_current(adapter) ||
        eglQuerySurface(
            adapter->egl_display,
            adapter->egl_surface,
            EGL_RENDER_BUFFER,
            &render_buffer) != EGL_TRUE ||
        render_buffer != EGL_BACK_BUFFER ||
        eglQuerySurface(
            adapter->egl_display,
            adapter->egl_surface,
            EGL_GL_COLORSPACE,
            &color_space) != EGL_TRUE ||
        color_space != EGL_GL_COLORSPACE_LINEAR ||
        eglSwapInterval(adapter->egl_display, adapter->swap_interval) !=
            EGL_TRUE) {
        adapter->context_invalid = true;
        return false;
    }
    /* The replacement drawable has no retained pixels, even when the
     * viewport and its framebuffer contents have not changed. */
    adapter->presentation_pending = true;
    return true;
}

static bool set_renderer_target(
    boxrig_x11_ogl_adapter *adapter,
    const boxrig_x11_ogl_framebuffer *framebuffer)
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
    boxrig_x11_ogl_adapter *adapter,
    const boxrig_x11_ogl_framebuffer *framebuffer)
{
    GLint destination_bottom;
    GLint destination_top;

    if (!ensure_current(adapter) || adapter->target_width == 0 ||
        adapter->target_height == 0 || adapter->viewport_width == 0 ||
        adapter->viewport_height == 0 ||
        adapter->viewport_width > adapter->target_width ||
        adapter->viewport_height > adapter->target_height ||
        framebuffer->width != adapter->viewport_width ||
        framebuffer->height != adapter->viewport_height ||
        adapter->viewport_x < 0 || adapter->viewport_y < 0 ||
        (unsigned int)adapter->viewport_x >
            adapter->target_width - adapter->viewport_width ||
        (unsigned int)adapter->viewport_y >
            adapter->target_height - adapter->viewport_height) {
        return false;
    }
    destination_bottom =
        (GLint)(adapter->target_height -
                (unsigned int)adapter->viewport_y -
                adapter->viewport_height);
    destination_top = destination_bottom + (GLint)adapter->viewport_height;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(
        0,
        0,
        (GLint)framebuffer->width,
        (GLint)framebuffer->height,
        adapter->viewport_x,
        destination_bottom,
        adapter->viewport_x + (GLint)adapter->viewport_width,
        destination_top,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST);
    if (glGetError() != GL_NO_ERROR ||
        eglSwapBuffers(adapter->egl_display, adapter->egl_surface) != EGL_TRUE) {
        EGLint error = eglGetError();

        if (error == EGL_CONTEXT_LOST || error == EGL_BAD_CONTEXT ||
            error == EGL_BAD_SURFACE) {
            adapter->context_invalid = true;
        }
        return false;
    }
    adapter->presentation_pending = false;
    return true;
}

static int adapter_prepare_font(
    void *context,
    const boxrig_x11_component_font_presentation *font)
{
    boxrig_x11_ogl_adapter *adapter = context;

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
    boxrig_x11_ogl_adapter *adapter = context;

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
    boxrig_x11_ogl_adapter *adapter = context;

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
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL || !ensure_current(adapter) ||
        (adapter->renderer != NULL &&
         boxrig_ogl_renderer_set_direction_policy(
             adapter->renderer,
             policy) == -1)) {
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
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL || state == NULL || !ensure_current(adapter) ||
        (adapter->renderer != NULL &&
         boxrig_ogl_renderer_apply_transparency_state(
             adapter->renderer,
             state) == -1)) {
        return -1;
    }
    adapter->transparency = *state;
    adapter->transparency_valid = true;
    return 0;
}

static int adapter_set_composited_scrollbar(
    void *context,
    bool enabled,
    unsigned int width)
{
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL || !ensure_current(adapter) ||
        (adapter->renderer != NULL &&
         boxrig_ogl_renderer_set_composited_scrollbar(
             adapter->renderer,
             enabled,
             width) == -1)) {
        return -1;
    }
    adapter->scrollbar_enabled = enabled;
    adapter->scrollbar_width = width;
    return 0;
}

static int adapter_set_composited_scrollbar_interaction(
    void *context,
    bool hovered,
    bool dragging)
{
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL || !ensure_current(adapter) ||
        boxrig_ogl_renderer_set_composited_scrollbar_interaction(
            adapter->renderer,
            hovered,
            dragging) == -1) {
        return -1;
    }
    adapter->scrollbar_hovered = hovered;
    adapter->scrollbar_dragging = dragging;
    return 0;
}

static int adapter_set_surface_size(
    void *context,
    unsigned int width,
    unsigned int height)
{
    boxrig_x11_ogl_adapter *adapter = context;
    boxrig_x11_ogl_framebuffer next;

    if (adapter == NULL || width == 0 || height == 0 ||
        adapter->candidate_active || !ensure_current(adapter)) {
        return -1;
    }
    if (adapter->committed.width == width &&
        adapter->committed.height == height) {
        return 0;
    }
    if (!create_framebuffer(width, height, &next)) {
        return -1;
    }
    if (!set_renderer_target(adapter, &next)) {
        destroy_framebuffer(&next);
        return -1;
    }
    destroy_framebuffer(&adapter->committed);
    adapter->committed = next;
    return 0;
}

static int adapter_set_target_extent(
    void *context,
    unsigned int width,
    unsigned int height)
{
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL || width == 0 || height == 0) {
        return -1;
    }
    if ((adapter->target_width != width || adapter->target_height != height) &&
        !recreate_window_surface(adapter)) {
        return -1;
    }
    adapter->target_width = width;
    adapter->target_height = height;
    return 0;
}

static int adapter_set_viewport(
    void *context,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL || x < 0 || y < 0 || width == 0 || height == 0 ||
        (unsigned int)x > adapter->target_width ||
        width > adapter->target_width - (unsigned int)x ||
        (unsigned int)y > adapter->target_height ||
        height > adapter->target_height - (unsigned int)y) {
        return -1;
    }
    adapter->viewport_x = x;
    adapter->viewport_y = y;
    adapter->viewport_width = width;
    adapter->viewport_height = height;
    return 0;
}

static int adapter_expose(
    void *context,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    boxrig_x11_ogl_adapter *adapter = context;

    (void)x;
    (void)y;
    if (adapter == NULL || width == 0 || height == 0) {
        return -1;
    }
    adapter->expose_pending = true;
    return 0;
}

static int adapter_prepare_surface_candidate(
    void *context,
    unsigned int width,
    unsigned int height)
{
    boxrig_x11_ogl_adapter *adapter = context;
    boxrig_renderer_target target;

    if (adapter == NULL || adapter->candidate_active || width == 0 ||
        height == 0 || !ensure_current(adapter) ||
        !create_framebuffer(width, height, &adapter->candidate) ||
        !set_renderer_target(adapter, &adapter->candidate)) {
        destroy_framebuffer(&adapter->candidate);
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

static int adapter_commit_surface_candidate(void *context)
{
    boxrig_x11_ogl_adapter *adapter = context;
    boxrig_x11_ogl_framebuffer previous;

    if (adapter == NULL || !adapter->candidate_active ||
        !adapter->candidate_frame_active ||
        !present_framebuffer(adapter, &adapter->candidate) ||
        boxrig_renderer_lifecycle_accept_frame(
            adapter->candidate_lifecycle,
            &adapter->candidate_token) != BOXRIG_RENDERER_OK) {
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
    return 0;
}

static void adapter_abort_surface_candidate(void *context)
{
    boxrig_x11_ogl_adapter *adapter = context;

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
    boxrig_composition *composition)
{
    boxrig_x11_ogl_adapter *adapter = context;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_frame_token token;
    boxrig_renderer_target target;
    boxrig_renderer_counters counters;
    bool present;
    int result = -1;

    if (adapter == NULL || view == NULL || composition == NULL) {
        return -1;
    }
    /* Keep both dirty and the committed image until the batch is complete. */
    if (boxrig_terminal_synchronized_output(boxrig_ui_view_terminal(view))) {
        return 0;
    }
    if (!ensure_current(adapter)) {
        return -1;
    }
    if (adapter->expose_pending) {
        if (boxrig_ogl_renderer_target_lost(adapter->renderer, view) !=
            BOXRIG_RENDERER_OK) {
            return -1;
        }
        adapter->expose_pending = false;
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
            &token) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_get_counters(lifecycle, &counters) !=
            BOXRIG_RENDERER_OK) {
        goto cleanup;
    }
    present = adapter->presentation_pending || counters.draw_count != 0 ||
              counters.dirty_rectangle_count != 0;
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
    boxrig_x11_ogl_adapter *adapter = context;

    if (adapter == NULL) {
        return;
    }
    release_context_resources(adapter);
    destroy_font_state(&adapter->font);
    if (adapter->display_acquired) {
        release_display(
            adapter->display,
            adapter->screen,
            adapter->egl_display);
    }
    free(adapter);
}

static const boxrig_x11_component_renderer_operations renderer_operations = {
    adapter_prepare_font,
    adapter_commit_font,
    adapter_abort_font,
    adapter_set_text_direction,
    adapter_apply_transparency_state,
    adapter_set_composited_scrollbar,
    adapter_set_composited_scrollbar_interaction,
    adapter_set_surface_size,
    adapter_set_target_extent,
    adapter_set_viewport,
    adapter_expose,
    adapter_prepare_surface_candidate,
    adapter_commit_surface_candidate,
    adapter_abort_surface_candidate,
    adapter_draw_view_composition,
    adapter_destroy};

static boxrig_x11_ogl_adapter *create_adapter(
    Display *display,
    Window window,
    int screen,
    EGLConfig egl_config,
    bool use_es,
    int alpha_size,
    int swap_interval)
{
    boxrig_x11_ogl_adapter *adapter = calloc(1, sizeof(*adapter));

    if (adapter == NULL) {
        return NULL;
    }
    adapter->display = display;
    adapter->window = window;
    adapter->screen = screen;
    adapter->egl_display = EGL_NO_DISPLAY;
    adapter->egl_surface = EGL_NO_SURFACE;
    adapter->egl_context = EGL_NO_CONTEXT;
    adapter->egl_config = egl_config;
    adapter->use_es = use_es;
    adapter->alpha_size = alpha_size;
    adapter->swap_interval = swap_interval;
    adapter->target_width = 1;
    adapter->target_height = 1;
    adapter->viewport_width = 1;
    adapter->viewport_height = 1;
    adapter->egl_display = acquire_display(display, screen);
    adapter->display_acquired = adapter->egl_display != EGL_NO_DISPLAY;
    if (!adapter->display_acquired) {
        free(adapter);
        return NULL;
    }
    /* The config handle belongs to this initialized EGLDisplay. */
    adapter->egl_config = egl_config;
    if (!create_context_and_renderer(adapter)) {
        adapter_destroy(adapter);
        return NULL;
    }
    return adapter;
}

boxrig_x11_opengl_component_config
boxrig_x11_opengl_component_config_default(void)
{
    boxrig_x11_opengl_component_config config;

    config.swap_interval = 1;
    return config;
}

boxrig_ui_status
boxrig_x11_opengl_terminal_component_get_window_requirements(
    Display *display,
    int screen,
    const boxrig_ui_transparency_config *transparency,
    boxrig_x11_opengl_window_requirements *requirements_out)
{
    XVisualInfo visual_template;
    XVisualInfo *visual_info = NULL;
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLint visual_id;
    int visual_count;
    int alpha_size;

    if (display == NULL || screen < 0 || screen >= ScreenCount(display) ||
        transparency == NULL || requirements_out == NULL ||
        (transparency->request != BOXRIG_UI_TRANSPARENCY_OPAQUE &&
         transparency->request != BOXRIG_UI_TRANSPARENCY_PREFERRED &&
         transparency->request != BOXRIG_UI_TRANSPARENCY_REQUIRED) ||
        (transparency->requested_scopes &
         ~(BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
           BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL)) != 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(requirements_out, 0, sizeof(*requirements_out));
    alpha_size =
        transparency->request != BOXRIG_UI_TRANSPARENCY_OPAQUE &&
                (transparency->requested_scopes &
                 BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0 &&
                compositor_active(display, screen)
            ? 8
            : 0;
    if (transparency->request == BOXRIG_UI_TRANSPARENCY_REQUIRED &&
        ((transparency->requested_scopes &
          BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL) != 0 ||
         ((transparency->requested_scopes &
           BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0 &&
          alpha_size != 8))) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    egl_display = acquire_display(display, screen);
    if (egl_display == EGL_NO_DISPLAY) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    egl_config = choose_available_config(
        egl_display,
        display,
        screen,
        alpha_size,
        &visual_id);
    if (egl_config == NULL && alpha_size == 8 &&
        transparency->request == BOXRIG_UI_TRANSPARENCY_PREFERRED) {
        alpha_size = 0;
        egl_config = choose_available_config(
            egl_display,
            display,
            screen,
            alpha_size,
            &visual_id);
    }
    if (egl_config == NULL) {
        release_display(display, screen, egl_display);
        return BOXRIG_UI_UNAVAILABLE;
    }
    memset(&visual_template, 0, sizeof(visual_template));
    visual_template.visualid = (VisualID)(unsigned long)visual_id;
    visual_template.screen = screen;
    visual_info = XGetVisualInfo(
        display,
        VisualIDMask | VisualScreenMask,
        &visual_template,
        &visual_count);
    if (visual_info == NULL || visual_count != 1) {
        if (visual_info != NULL) {
            XFree(visual_info);
        }
        release_display(display, screen, egl_display);
        return BOXRIG_UI_UNAVAILABLE;
    }
    requirements_out->visual = visual_info->visual;
    requirements_out->depth = visual_info->depth;
    requirements_out->premultiplied_argb32 = alpha_size == 8;
    XFree(visual_info);
    release_display(display, screen, egl_display);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_opengl_terminal_component_create_child(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_x11_opengl_component_config opengl_config =
        boxrig_x11_opengl_component_config_default();

    return boxrig_x11_opengl_terminal_component_create_child_with_config(
        display,
        parent,
        model,
        config,
        &opengl_config,
        component_out);
}

static boxrig_ui_status create_child_with_api(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    bool use_es,
    boxrig_x11_terminal_component **component_out)
{
    XWindowAttributes parent_attributes;
    XVisualInfo visual_template;
    XVisualInfo *visual_info = NULL;
    XSetWindowAttributes attributes;
    boxrig_x11_ogl_adapter *adapter = NULL;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLConfig egl_config;
    EGLint visual_id;
    Colormap colormap = None;
    Window window = None;
    int visual_count;
    int alpha_size;
    boxrig_ui_status status = BOXRIG_UI_UNAVAILABLE;

    if (display == NULL || parent == None || model == NULL ||
        !valid_component_config(config, false) || opengl_config == NULL ||
        component_out == NULL || boxrig_ui_model_terminal(model) == NULL ||
        (opengl_config->swap_interval != 0 &&
         opengl_config->swap_interval != 1)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (XGetWindowAttributes(display, parent, &parent_attributes) == 0) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    alpha_size = config->transparency.request != BOXRIG_UI_TRANSPARENCY_OPAQUE &&
                         (config->transparency.requested_scopes &
                          BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0 &&
                         compositor_active(display, XScreenNumberOfScreen(
                                                        parent_attributes.screen))
                     ? 8
                     : 0;
    if (config->transparency.request == BOXRIG_UI_TRANSPARENCY_REQUIRED &&
        (config->transparency.requested_scopes &
         BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL) != 0) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    egl_display = acquire_display(
        display,
        XScreenNumberOfScreen(parent_attributes.screen));
    if (egl_display == EGL_NO_DISPLAY) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    egl_config = choose_config(
        egl_display,
        display,
        XScreenNumberOfScreen(parent_attributes.screen),
        alpha_size,
        0,
        use_es,
        &visual_id);
    if (egl_config == NULL && alpha_size == 8 &&
        config->transparency.request == BOXRIG_UI_TRANSPARENCY_PREFERRED) {
        alpha_size = 0;
        egl_config = choose_config(
            egl_display,
            display,
            XScreenNumberOfScreen(parent_attributes.screen),
            alpha_size,
            0,
            use_es,
            &visual_id);
    }
    if (egl_config == NULL) {
        goto cleanup;
    }
    memset(&visual_template, 0, sizeof(visual_template));
    visual_template.visualid = (VisualID)(unsigned long)visual_id;
    visual_template.screen = XScreenNumberOfScreen(parent_attributes.screen);
    visual_info = XGetVisualInfo(
        display,
        VisualIDMask | VisualScreenMask,
        &visual_template,
        &visual_count);
    if (visual_info == NULL || visual_count != 1 ||
        visual_info->depth != (alpha_size == 8 ? 32 : 24)) {
        goto cleanup;
    }
    colormap = XCreateColormap(
        display,
        parent_attributes.root,
        visual_info->visual,
        AllocNone);
    if (colormap == None) {
        goto cleanup;
    }
    memset(&attributes, 0, sizeof(attributes));
    attributes.colormap = colormap;
    attributes.border_pixel = 0;
    attributes.event_mask =
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
        ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
        PropertyChangeMask | StructureNotifyMask;
    window = XCreateWindow(
        display,
        parent,
        0,
        0,
        1,
        1,
        0,
        visual_info->depth,
        InputOutput,
        visual_info->visual,
        CWColormap | CWBorderPixel | CWEventMask,
        &attributes);
    if (window == None) {
        goto cleanup;
    }
    adapter = create_adapter(
        display,
        window,
        XScreenNumberOfScreen(parent_attributes.screen),
        egl_config,
        use_es,
        alpha_size,
        opengl_config->swap_interval);
    if (adapter == NULL) {
        goto cleanup;
    }
    /* Drop the factory's display reference. The adapter retains its own. */
    release_display(
        display,
        XScreenNumberOfScreen(parent_attributes.screen),
        egl_display);
    egl_display = EGL_NO_DISPLAY;
    status = boxrig_x11_terminal_component_create_adopted_child(
        display,
        parent,
        window,
        colormap,
        model,
        config,
        adapter,
        &renderer_operations,
        component_out);
    /* The adopted-child transaction owns every native resource after the
     * factory-created target has passed its validation, including rollback. */
    adapter = NULL;
    window = None;
    colormap = None;

cleanup:
    if (adapter != NULL) {
        adapter_destroy(adapter);
    }
    if (window != None) {
        XDestroyWindow(display, window);
    }
    if (colormap != None) {
        XFreeColormap(display, colormap);
    }
    if (visual_info != NULL) {
        XFree(visual_info);
    }
    if (egl_display != EGL_NO_DISPLAY) {
        release_display(
            display,
            XScreenNumberOfScreen(parent_attributes.screen),
            egl_display);
    }
    return status;
}

boxrig_ui_status boxrig_x11_opengl_terminal_component_create_child_with_config(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_ui_status status = create_child_with_api(
        display, parent, model, config, opengl_config, true, component_out);
    if (status != BOXRIG_UI_UNAVAILABLE) return status;
    /* Each attempt owns a complete child/renderer transaction, including
     * rollback when context creation succeeds but renderer setup fails. */
    return create_child_with_api(
        display, parent, model, config, opengl_config, false, component_out);
}

static boxrig_ui_status create_on_target_with_api(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    bool use_es,
    boxrig_x11_terminal_component **component_out)
{
    XWindowAttributes attributes;
    XWindowAttributes input_attributes;
    boxrig_x11_ogl_adapter *adapter;
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLint visual_id;
    VisualID required_visual;
    int screen;
    int alpha_size;
    boxrig_ui_status status;

    if (display == NULL || target == NULL || model == NULL ||
        !valid_component_config(config, true) ||
        opengl_config == NULL || component_out == NULL ||
        boxrig_ui_model_terminal(model) == NULL ||
        (opengl_config->swap_interval != 0 &&
         opengl_config->swap_interval != 1)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (target->presentation_window == None ||
        target->input_window == None ||
        XGetWindowAttributes(
            display,
            target->presentation_window,
            &attributes) == 0 ||
        XGetWindowAttributes(
            display,
            target->input_window,
            &input_attributes) == 0 ||
        attributes.class != InputOutput ||
        input_attributes.class != InputOutput ||
        attributes.root != input_attributes.root ||
        attributes.screen != input_attributes.screen ||
        attributes.visual != input_attributes.visual ||
        attributes.depth != input_attributes.depth ||
        attributes.colormap != input_attributes.colormap) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    screen = XScreenNumberOfScreen(attributes.screen);
    required_visual = XVisualIDFromVisual(attributes.visual);
    alpha_size = attributes.depth == 32 ? 8 : 0;
    if ((alpha_size == 8 && !compositor_active(display, screen)) ||
        (attributes.depth != 24 && attributes.depth != 32)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    egl_display = acquire_display(display, screen);
    if (egl_display == EGL_NO_DISPLAY) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    egl_config = choose_config(
        egl_display,
        display,
        screen,
        alpha_size,
        required_visual,
        use_es,
        &visual_id);
    if (egl_config == NULL ||
        (VisualID)(unsigned long)visual_id != required_visual) {
        release_display(display, screen, egl_display);
        return BOXRIG_UI_UNAVAILABLE;
    }
    adapter = create_adapter(
        display,
        target->presentation_window,
        screen,
        egl_config,
        use_es,
        alpha_size,
        opengl_config->swap_interval);
    release_display(display, screen, egl_display);
    if (adapter == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    adapter->target_width = (unsigned int)attributes.width;
    adapter->target_height = (unsigned int)attributes.height;
    status = boxrig_x11_terminal_component_create_on_target_with_renderer(
        display,
        target,
        model,
        config,
        adapter,
        &renderer_operations,
        component_out);
    /* The component transaction owns adapter after the validated call,
     * including allocation and initialization rollback. */
    adapter = NULL;
    return status;
}

boxrig_ui_status boxrig_x11_opengl_terminal_component_create_on_target(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_ui_status status = create_on_target_with_api(
        display, target, model, config, opengl_config, true, component_out);
    if (status != BOXRIG_UI_UNAVAILABLE) return status;
    return create_on_target_with_api(
        display, target, model, config, opengl_config, false, component_out);
}
