/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_OPENGL_COMPONENT_H
#define BOXRIG_X11_OPENGL_COMPONENT_H

#include "boxrig/x11/component.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configuration for the optional EGL/OpenGL or OpenGL ES X11 component factory. */
typedef struct boxrig_x11_opengl_component_config {
    /** Requested EGL swap interval. Zero and one are accepted. */
    int swap_interval;
} boxrig_x11_opengl_component_config;

/** X11 Visual requirements for a caller-owned EGL presentation Window. */
typedef struct boxrig_x11_opengl_window_requirements {
    /** Display-owned Visual compatible with the selected EGLConfig. */
    Visual *visual;
    /** Window depth matching visual. */
    int depth;
    /** Whether the Visual preserves premultiplied ARGB32 alpha. */
    bool premultiplied_argb32;
} boxrig_x11_opengl_window_requirements;

/**
 * Returns conservative OpenGL component defaults.
 *
 * @return A configuration using swap interval one.
 */
boxrig_x11_opengl_component_config
boxrig_x11_opengl_component_config_default(void);

/**
 * Selects a Visual suitable for a caller-owned EGL presentation Window.
 *
 * @param display Caller-owned X11 connection.
 * @param screen Screen index for the future Window.
 * @param transparency Component transparency request used for alpha selection.
 * @param requirements_out Receives display-owned Visual requirements.
 * @return BOXRIG_UI_UNAVAILABLE when no compatible EGLConfig, Visual and
 * OpenGL ES 3.0 or OpenGL 3.3 core context can satisfy the request,
 * otherwise a portable UI status.
 *
 * @remarks The caller creates and owns a Colormap for the returned Visual,
 * creates the Window with the matching depth, then passes that Window to
 * boxrig_x11_opengl_terminal_component_create_on_target(). The result borrows
 * only Xlib objects and remains valid while display is open. Selection prefers
 * OpenGL ES and probes context creation without making the probe current.
 */
boxrig_ui_status
boxrig_x11_opengl_terminal_component_get_window_requirements(
    Display *display,
    int screen,
    const boxrig_ui_transparency_config *transparency,
    boxrig_x11_opengl_window_requirements *requirements_out);

/**
 * Creates an EGL/OpenGL terminal child inside a caller-owned parent Window.
 *
 * @param display Caller-owned X11 connection that outlives the component.
 * @param parent Caller-owned parent that outlives the component.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Portable component configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE when a compatible EGL target cannot be made,
 * otherwise a portable UI status.
 *
 * @remarks This link-time factory owns an EGL context, EGL window surface,
 * compatible-Visual child Window, and GPU presentation resources. It never
 * dispatches or waits for X11 events. The regular X11 factory remains the
 * default software renderer. Creation tries OpenGL ES 3.0 first, then OpenGL
 * 3.3 core when ES configuration, context or renderer setup is unavailable.
 */
boxrig_ui_status boxrig_x11_opengl_terminal_component_create_child(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out);

/**
 * Creates a configured EGL/OpenGL terminal child.
 *
 * @param display Caller-owned X11 connection that outlives the component.
 * @param parent Caller-owned parent that outlives the component.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Portable component configuration copied during creation.
 * @param opengl_config EGL presentation configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE when a compatible EGL target cannot be made,
 * otherwise a portable UI status.
 */
boxrig_ui_status boxrig_x11_opengl_terminal_component_create_child_with_config(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    boxrig_x11_terminal_component **component_out);

/**
 * Creates an EGL/OpenGL component on caller-owned compatible X11 targets.
 *
 * @param display Caller-owned X11 connection that outlives the component.
 * @param target Caller-owned presentation and input targets.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Portable component configuration copied during creation.
 * @param opengl_config EGL presentation configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE when the target Visual does not exactly match
 * an EGL config or neither OpenGL ES 3.0 nor OpenGL 3.3 core can be initialized,
 * otherwise a portable UI status.
 *
 * @remarks The adapter never changes or destroys either borrowed Window and
 * does not create an intermediate native Window or software copy. Both API
 * attempts must match the borrowed Visual; OpenGL ES is preferred.
 */
boxrig_ui_status boxrig_x11_opengl_terminal_component_create_on_target(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    boxrig_x11_terminal_component **component_out);

#ifdef __cplusplus
}
#endif

#endif
