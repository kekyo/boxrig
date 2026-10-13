/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/opengl_component.h"

#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <X11/Xlib.h>

#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(                                                           \
                stderr,                                                        \
                "FAIL %s:%d: %s\n",                                           \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #condition);                                                   \
            passed = false;                                                    \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

static bool fail_next_context_create;
static bool fail_all_context_creates;
static bool reject_gles_contexts;
static bool reject_gles_renderer;
static EGLint excluded_renderable_types;
static size_t gles_context_attempts;
static size_t desktop_context_attempts;
static bool fail_next_make_current;
static bool fail_next_swap;
static EGLint failed_swap_error = EGL_SUCCESS;
static size_t context_create_count;
static size_t swap_count;
static size_t font_config_parse_count;
static bool capture_next_swap;
static GLint capture_x;
static GLint capture_y;
static uint8_t captured_swap_pixel[4];
static bool captured_swap_pixel_valid;

EGLContext __real_eglCreateContext(
    EGLDisplay display,
    EGLConfig config,
    EGLContext share_context,
    const EGLint *attributes);

EGLBoolean __real_eglMakeCurrent(
    EGLDisplay display,
    EGLSurface draw,
    EGLSurface read,
    EGLContext context);

EGLBoolean __real_eglSwapBuffers(EGLDisplay display, EGLSurface surface);

EGLint __real_eglGetError(void);

EGLBoolean __real_eglGetConfigAttrib(
    EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value);

__eglMustCastToProperFunctionPointerType __real_eglGetProcAddress(const char *name);

EGLBoolean __wrap_eglGetConfigAttrib(
    EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value)
{
    EGLBoolean result = __real_eglGetConfigAttrib(display, config, attribute, value);
    if (result == EGL_TRUE && attribute == EGL_RENDERABLE_TYPE)
        *value &= ~excluded_renderable_types;
    return result;
}

__eglMustCastToProperFunctionPointerType __wrap_eglGetProcAddress(const char *name)
{
    if (reject_gles_renderer && eglQueryAPI() == EGL_OPENGL_ES_API &&
        strcmp(name, "glCreateShader") == 0) return NULL;
    return __real_eglGetProcAddress(name);
}

FcBool __real_FcConfigParseAndLoad(
    FcConfig *config,
    const FcChar8 *file,
    FcBool complain);

FcBool __wrap_FcConfigParseAndLoad(
    FcConfig *config,
    const FcChar8 *file,
    FcBool complain)
{
    font_config_parse_count++;
    return __real_FcConfigParseAndLoad(config, file, complain);
}

EGLContext __wrap_eglCreateContext(
    EGLDisplay display,
    EGLConfig config,
    EGLContext share_context,
    const EGLint *attributes)
{
    context_create_count++;
    bool is_gles = eglQueryAPI() == EGL_OPENGL_ES_API;
    if (is_gles) gles_context_attempts++;
    else desktop_context_attempts++;
    if (fail_all_context_creates || (is_gles && reject_gles_contexts))
        return EGL_NO_CONTEXT;
    if (fail_next_context_create) {
        fail_next_context_create = false;
        return EGL_NO_CONTEXT;
    }
    return __real_eglCreateContext(
        display,
        config,
        share_context,
        attributes);
}

EGLBoolean __wrap_eglMakeCurrent(
    EGLDisplay display,
    EGLSurface draw,
    EGLSurface read,
    EGLContext context)
{
    if (fail_next_make_current) {
        fail_next_make_current = false;
        return EGL_FALSE;
    }
    return __real_eglMakeCurrent(display, draw, read, context);
}

EGLBoolean __wrap_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    swap_count++;
    if (fail_next_swap) {
        fail_next_swap = false;
        return EGL_FALSE;
    }
    if (capture_next_swap) {
        GLint previous_framebuffer;
        GLint previous_buffer;

        capture_next_swap = false;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_framebuffer);
        glGetIntegerv(GL_READ_BUFFER, &previous_buffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(
            capture_x,
            capture_y,
            1,
            1,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            captured_swap_pixel);
        captured_swap_pixel_valid = glGetError() == GL_NO_ERROR;
        glBindFramebuffer(
            GL_READ_FRAMEBUFFER,
            (GLuint)previous_framebuffer);
        glReadBuffer((GLenum)previous_buffer);
    }
    return __real_eglSwapBuffers(display, surface);
}

EGLint __wrap_eglGetError(void)
{
    if (failed_swap_error != EGL_SUCCESS) {
        EGLint error = failed_swap_error;

        failed_swap_error = EGL_SUCCESS;
        return error;
    }
    return __real_eglGetError();
}

static unsigned int child_count(Display *display, Window window)
{
    Window root;
    Window parent;
    Window *children = NULL;
    unsigned int count = 0;

    if (!XQueryTree(display, window, &root, &parent, &children, &count)) {
        return UINT_MAX;
    }
    if (children != NULL) {
        XFree(children);
    }
    return count;
}

static bool feed_bytes(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t consumed = 0;

    return boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
               BOXRIG_STATUS_OK &&
           consumed == length;
}

int main(int argument_count, char **arguments)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_x11_opengl_component_config opengl_config =
        boxrig_x11_opengl_component_config_default();
    boxrig_x11_terminal_component *component = NULL;
    boxrig_x11_terminal_component *borrowed_component = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view;
    boxrig_font_effective_metrics metrics;
    boxrig_font_specification font;
    boxrig_font_specification reported_font;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_layout_result layout;
    boxrig_ui_dirty dirty;
    boxrig_x11_terminal_borrowed_target borrowed_target;
    Display *display = NULL;
    Window parent = None;
    Window borrowed = None;
    Window incompatible = None;
    XSetWindowAttributes parent_attributes;
    XSetWindowAttributes input_only_attributes;
    XWindowAttributes attributes;
    unsigned int children_before;
    size_t contexts_before;
    size_t swaps_before;
    boxrig_ui_status status;
    const GLubyte *gl_vendor;
    const GLubyte *gl_renderer;
    const GLubyte *gl_version;
    boxrig_x11_opengl_window_requirements requirements;
    bool expected_gles = true;
    bool native_api = false;
    bool compositor;
    bool passed = true;

    CHECK(argument_count == 1 || argument_count == 2);
    if (argument_count == 2) {
        if (strcmp(arguments[1], "native") == 0) {
            native_api = true;
        } else if (strcmp(arguments[1], "desktop-context") == 0) {
            reject_gles_contexts = true;
            expected_gles = false;
        } else if (strcmp(arguments[1], "desktop-renderer") == 0) {
            reject_gles_renderer = true;
            expected_gles = false;
        } else if (strcmp(arguments[1], "desktop-config") == 0) {
            excluded_renderable_types = EGL_OPENGL_ES3_BIT;
            expected_gles = false;
        } else if (strcmp(arguments[1], "gles-config") == 0) {
            excluded_renderable_types = EGL_OPENGL_BIT;
        } else CHECK(false);
    }
    CHECK(opengl_config.swap_interval == 1);
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    memset(&parent_attributes, 0, sizeof(parent_attributes));
    parent_attributes.override_redirect = True;
    parent_attributes.background_pixel = BlackPixel(
        display,
        DefaultScreen(display));
    parent = XCreateWindow(
        display,
        DefaultRootWindow(display),
        100,
        100,
        320,
        160,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWOverrideRedirect | CWBackPixel,
        &parent_attributes);
    CHECK(parent != None);
    XMapWindow(display, parent);
    XRaiseWindow(display, parent);
    XSync(display, False);

    terminal_config.rows = 2;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;

    children_before = child_count(display, parent);
    CHECK(children_before != UINT_MAX);
    {
        char selection_name[64];
        Atom selection;

        CHECK(snprintf(
                  selection_name,
                  sizeof(selection_name),
                  "_NET_WM_CM_S%d",
                  DefaultScreen(display)) > 0);
        selection = XInternAtom(display, selection_name, True);
        compositor = selection != None &&
                     XGetSelectionOwner(display, selection) != None;
    }
    opengl_config.swap_interval = 2;
    CHECK(boxrig_x11_opengl_terminal_component_create_child_with_config(
              display,
              parent,
              model,
              &config,
              &opengl_config,
              &component) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(component == NULL);
    opengl_config = boxrig_x11_opengl_component_config_default();
    fail_all_context_creates = true;
    CHECK(boxrig_x11_opengl_terminal_component_create_child(
              display,
              parent,
              model,
              &config,
              &component) == BOXRIG_UI_UNAVAILABLE);
    CHECK(component == NULL);
    CHECK(boxrig_x11_opengl_terminal_component_get_window_requirements(
              display, DefaultScreen(display), &config.transparency,
              &requirements) == BOXRIG_UI_UNAVAILABLE);
    fail_all_context_creates = false;
    XSync(display, False);
    CHECK(child_count(display, parent) == children_before);

    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    config.transparency.background_opacity = 128;
    status = boxrig_x11_opengl_terminal_component_create_child(
        display,
        parent,
        model,
        &config,
        &component);
    CHECK(status == BOXRIG_UI_UNAVAILABLE ||
          (compositor && status == BOXRIG_UI_OK));
    if (component != NULL) {
        CHECK((boxrig_x11_terminal_component_capabilities(component) &
               BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA) != 0);
        CHECK(boxrig_x11_terminal_component_get_effective_metrics(
                  component,
                  &metrics) == BOXRIG_FONT_CATALOG_OK);
        memset(&allocation, 0, sizeof(allocation));
        allocation.width = metrics.cell_width * terminal_config.columns;
        allocation.height = metrics.cell_height * terminal_config.rows;
        allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
        CHECK(boxrig_x11_terminal_component_allocate(
                  component,
                  &allocation,
                  &layout) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_set_visible(component, true) ==
              BOXRIG_UI_OK);
        /* Sample inside an empty cell so the opaque cursor outline cannot
         * replace the transparent background at a rasterization boundary. */
        capture_x = (GLint)(metrics.cell_width + metrics.cell_width / 2);
        capture_y =
            (GLint)(allocation.height - metrics.cell_height / 2 - 1);
        captured_swap_pixel_valid = false;
        capture_next_swap = true;
        CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
              BOXRIG_UI_OK);
        CHECK(captured_swap_pixel_valid);
        CHECK(captured_swap_pixel[0] <= captured_swap_pixel[3]);
        CHECK(captured_swap_pixel[1] <= captured_swap_pixel[3]);
        CHECK(captured_swap_pixel[2] <= captured_swap_pixel[3]);
        if (captured_swap_pixel[3] != 128) {
            fprintf(
                stderr,
                "X11 OpenGL transparent back buffer pixel: %u/%u/%u/%u\n",
                (unsigned int)captured_swap_pixel[0],
                (unsigned int)captured_swap_pixel[1],
                (unsigned int)captured_swap_pixel[2],
                (unsigned int)captured_swap_pixel[3]);
        }
        CHECK(captured_swap_pixel[3] == 128);
        boxrig_x11_terminal_component_destroy(component);
        component = NULL;
    }
    XSync(display, False);
    CHECK(child_count(display, parent) == children_before);
    config = boxrig_x11_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;

    CHECK(boxrig_x11_opengl_terminal_component_get_window_requirements(
              display, DefaultScreen(display), &config.transparency,
              &requirements) == BOXRIG_UI_OK);
    CHECK(requirements.visual != NULL && requirements.depth == 24);
    opengl_config.swap_interval = 0;
    font_config_parse_count = 0;
    CHECK(boxrig_x11_opengl_terminal_component_create_child_with_config(
              display,
              parent,
              model,
              &config,
              &opengl_config,
              &component) == BOXRIG_UI_OK);
    CHECK(component != NULL);
    CHECK(font_config_parse_count == (reject_gles_renderer ? 2u : 1u));
    gl_vendor = glGetString(GL_VENDOR);
    gl_renderer = glGetString(GL_RENDERER);
    gl_version = glGetString(GL_VERSION);
    CHECK(gl_vendor != NULL && gl_renderer != NULL);
    CHECK(gl_version != NULL);
    /* Physical drivers may only provide desktop GL. The controlled Mesa
     * cases above enforce ES preference and both fallback failure points. */
    if (native_api)
        expected_gles = strncmp((const char *)gl_version, "OpenGL ES ", 10) == 0;
    CHECK(native_api ||
          (strncmp((const char *)gl_version, "OpenGL ES ", 10) == 0) == expected_gles);
    printf(
        "OpenGL provider: %s / %s / %s\n",
        (const char *)gl_vendor,
        (const char *)gl_renderer,
        (const char *)gl_version);
    CHECK(boxrig_x11_terminal_component_owned_window(component) != None);
    CHECK(boxrig_x11_terminal_component_presentation_window(component) ==
          boxrig_x11_terminal_component_input_window(component));
    CHECK(XGetWindowAttributes(
        display,
        boxrig_x11_terminal_component_owned_window(component),
        &attributes));
    CHECK(attributes.class == InputOutput && attributes.colormap != None);
    view = boxrig_x11_terminal_component_view(component);
    CHECK(view != NULL);
    CHECK(boxrig_x11_terminal_component_get_font_specification(
              component,
              &font) == BOXRIG_UI_OK);
    font.size_dip = 18.0f;
    fail_next_make_current = true;
    fail_next_context_create = true;
    CHECK(boxrig_x11_terminal_component_set_font_specification(
              component,
              &font) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_x11_terminal_component_get_font_specification(
              component,
              &reported_font) == BOXRIG_UI_OK);
    CHECK(reported_font.size_dip == 12.0f);
    font = reported_font;
    font.size_dip = 24.0f;
    CHECK(boxrig_x11_terminal_component_set_font_specification(
              component,
              &font) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              component,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * terminal_config.columns;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_x11_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    XSync(display, False);
    CHECK(boxrig_x11_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    CHECK(feed_bytes(
        terminal,
        (const uint8_t *)"\033[48;2;18;52;86m  \033[0mfirst",
        sizeof("\033[48;2;18;52;86m  \033[0mfirst") - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    failed_swap_error = EGL_BAD_ALLOC;
    fail_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    capture_x = (GLint)(metrics.cell_width + metrics.cell_width / 2);
    capture_y = (GLint)(allocation.height - metrics.cell_height / 2 - 1);
    captured_swap_pixel_valid = false;
    capture_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    CHECK(swap_count >= 2);
    CHECK(captured_swap_pixel_valid);
    CHECK(captured_swap_pixel[0] == 0x12);
    CHECK(captured_swap_pixel[1] == 0x34);
    CHECK(captured_swap_pixel[2] == 0x56);

    /* Partial synchronized output must keep the committed frame visible. */
    {
        static const uint8_t partial[] =
            "\033[?2026h\033[H\033[48;2;153;51;34m  ";
        static const uint8_t complete[] =
            "\033[H\033[48;2;18;52;86m  \033[0m\033[?2026l";
        swaps_before = swap_count;
        CHECK(feed_bytes(terminal, partial, sizeof(partial) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
              BOXRIG_UI_OK);
        CHECK(swap_count == swaps_before);
        CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
        CHECK(feed_bytes(terminal, complete, sizeof(complete) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        captured_swap_pixel_valid = false;
        capture_next_swap = true;
        CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
              BOXRIG_UI_OK);
        CHECK(captured_swap_pixel_valid);
        CHECK(captured_swap_pixel[0] == 0x12 && captured_swap_pixel[1] == 0x34 &&
              captured_swap_pixel[2] == 0x56);
        CHECK(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    }

    CHECK(feed_bytes(terminal, (const uint8_t *)"\rsecond", 7));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    contexts_before = context_create_count;
    failed_swap_error = EGL_CONTEXT_LOST;
    fail_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(context_create_count == contexts_before + 1);
    CHECK(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));

    CHECK(feed_bytes(terminal, (const uint8_t *)"!", 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    contexts_before = context_create_count;
    fail_next_make_current = true;
    CHECK(boxrig_x11_terminal_component_frame(component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(context_create_count == contexts_before + 1);
    boxrig_x11_terminal_component_destroy(component);
    component = NULL;

    borrowed = XCreateSimpleWindow(
        display,
        parent,
        0,
        0,
        160,
        80,
        0,
        0,
        0);
    CHECK(borrowed != None);
    XSelectInput(
        display,
        borrowed,
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
            PropertyChangeMask | StructureNotifyMask);
    XMapWindow(display, borrowed);
    XSync(display, False);
    borrowed_target.presentation_window = borrowed;
    borrowed_target.input_window = borrowed;
    opengl_config.swap_interval = 1;
    CHECK(boxrig_x11_opengl_terminal_component_create_on_target(
              display,
              &borrowed_target,
              model,
              &config,
              &opengl_config,
              &borrowed_component) == BOXRIG_UI_OK);
    CHECK(borrowed_component != NULL);
    gl_version = glGetString(GL_VERSION);
    CHECK(gl_version != NULL);
    CHECK(native_api ||
          (strncmp((const char *)gl_version, "OpenGL ES ", 10) == 0) == expected_gles);
    CHECK(boxrig_x11_terminal_component_owned_window(borrowed_component) ==
          None);
    CHECK(boxrig_x11_terminal_component_presentation_window(
              borrowed_component) == borrowed);
    CHECK(boxrig_x11_terminal_component_get_effective_metrics(
              borrowed_component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = metrics.cell_width * terminal_config.columns;
    allocation.height = metrics.cell_height * terminal_config.rows;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_x11_terminal_component_allocate(
              borrowed_component, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_set_visible(borrowed_component, true) ==
          BOXRIG_UI_OK);
    CHECK(feed_bytes(terminal,
        (const uint8_t *)"\033[2J\033[H\033[?25l\033[48;2;18;52;86m  \033[0m",
        sizeof("\033[2J\033[H\033[?25l\033[48;2;18;52;86m  \033[0m") - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    capture_x = (GLint)(metrics.cell_width + metrics.cell_width / 2);
    capture_y = (GLint)(80 - metrics.cell_height / 2 - 1);
    captured_swap_pixel_valid = false;
    capture_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(borrowed_component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(captured_swap_pixel_valid);
    CHECK(captured_swap_pixel[0] == 0x12 && captured_swap_pixel[1] == 0x34 &&
          captured_swap_pixel[2] == 0x56);
    /* The host's drawable changes independently of the retained viewport.
     * Restoring its pixels must not require another terminal byte or Expose. */
    XResizeWindow(display, borrowed, 160, 96);
    XSync(display, False);
    CHECK(boxrig_x11_terminal_component_set_target_extent(
              borrowed_component, 160, 96) == BOXRIG_UI_OK);
    capture_y = (GLint)(96 - metrics.cell_height / 2 - 1);
    captured_swap_pixel_valid = false;
    capture_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(borrowed_component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(captured_swap_pixel_valid);
    CHECK(captured_swap_pixel[0] == 0x12 && captured_swap_pixel[1] == 0x34 &&
          captured_swap_pixel[2] == 0x56);
    swaps_before = swap_count;
    CHECK(boxrig_x11_terminal_component_set_target_extent(
              borrowed_component, 160, 96) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_terminal_component_frame(borrowed_component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(swap_count == swaps_before);
    /* A failed presentation must leave the retained image available to retry. */
    XResizeWindow(display, borrowed, 160, 112);
    XSync(display, False);
    CHECK(boxrig_x11_terminal_component_set_target_extent(
              borrowed_component, 160, 112) == BOXRIG_UI_OK);
    failed_swap_error = EGL_BAD_ALLOC;
    fail_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(borrowed_component, &frame_result) ==
          BOXRIG_UI_UNAVAILABLE);
    capture_y = (GLint)(112 - metrics.cell_height / 2 - 1);
    captured_swap_pixel_valid = false;
    capture_next_swap = true;
    CHECK(boxrig_x11_terminal_component_frame(borrowed_component, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(captured_swap_pixel_valid);
    CHECK(captured_swap_pixel[0] == 0x12 && captured_swap_pixel[1] == 0x34 &&
          captured_swap_pixel[2] == 0x56);
    boxrig_x11_terminal_component_destroy(borrowed_component);
    borrowed_component = NULL;
    CHECK(XGetWindowAttributes(display, borrowed, &attributes));

    memset(&input_only_attributes, 0, sizeof(input_only_attributes));
    incompatible = XCreateWindow(
        display,
        parent,
        0,
        0,
        8,
        8,
        0,
        0,
        InputOnly,
        CopyFromParent,
        0,
        &input_only_attributes);
    CHECK(incompatible != None);
    borrowed_target.presentation_window = incompatible;
    borrowed_target.input_window = incompatible;
    CHECK(boxrig_x11_opengl_terminal_component_create_on_target(
              display,
              &borrowed_target,
              model,
              &config,
              &opengl_config,
              &borrowed_component) == BOXRIG_UI_UNAVAILABLE);
    CHECK(borrowed_component == NULL);
    CHECK(XGetWindowAttributes(display, incompatible, &attributes));
    CHECK(native_api || (excluded_renderable_types & EGL_OPENGL_ES3_BIT) != 0 ||
          gles_context_attempts != 0);
    CHECK(expected_gles || desktop_context_attempts != 0);
    printf("PASS X11 EGL/%s component target and acceptance contracts (%s)\n",
        expected_gles ? "OpenGL ES" : "OpenGL",
        argument_count == 2 ? arguments[1] : "auto");

cleanup:
    boxrig_x11_terminal_component_destroy(borrowed_component);
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (incompatible != None && display != NULL) {
        XDestroyWindow(display, incompatible);
    }
    if (borrowed != None && display != NULL) {
        XDestroyWindow(display, borrowed);
    }
    if (parent != None && display != NULL) {
        XDestroyWindow(display, parent);
    }
    if (display != NULL) {
        XSync(display, False);
        XCloseDisplay(display);
    }
    cairo_debug_reset_static_data();
    FcFini();
    return passed ? 0 : 1;
}
