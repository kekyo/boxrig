/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_OPENGL_COMPONENT_H
#define BOXRIG_WAYLAND_OPENGL_COMPONENT_H

#include "boxrig/wayland.h"

/** Configuration for the optional EGL/OpenGL Wayland component factory. */
typedef struct boxrig_wayland_opengl_component_config {
    /** Requested EGL swap interval. Zero and one are accepted. */
    int swap_interval;
} boxrig_wayland_opengl_component_config;

/** Observable state of a Wayland EGL/OpenGL presentation adapter. */
typedef struct boxrig_wayland_opengl_component_diagnostic {
    /** Monotonic managed EGL context generation. */
    uint64_t context_generation;
    /** Monotonic renderer target generation. */
    uint64_t target_generation;
    /** Number of successful EGL buffer swaps. */
    uint64_t accepted_swap_count;
    /** Number of full redraws caused by creation, resize, or target loss. */
    uint64_t full_redraw_count;
    /** Current physical wl_egl_window width. */
    unsigned int width;
    /** Current physical wl_egl_window height. */
    unsigned int height;
    /** Whether the selected EGLConfig has eight alpha bits. */
    bool alpha_target;
    /** Whether a context or surface recreation is pending. */
    bool target_lost;
    /** Whether partial swap is enabled for this adapter. */
    bool partial_swap_enabled;
} boxrig_wayland_opengl_component_diagnostic;

/**
 * Returns conservative Wayland OpenGL component defaults.
 *
 * @return A configuration using swap interval zero because wl_surface.frame
 * provides the component's presentation pacing.
 */
boxrig_wayland_opengl_component_config
boxrig_wayland_opengl_component_config_default(void);

/**
 * Creates an EGL/OpenGL terminal component on a managed child wl_surface.
 *
 * @param context Existing Wayland context owning protocol dispatch and input.
 * @param parent Caller-owned parent wl_surface that outlives the component.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Portable component configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE when a compatible EGL target cannot be made,
 * otherwise a portable UI status.
 *
 * @remarks The adapter owns only its EGLDisplay reference, EGLContext,
 * EGLSurface, wl_egl_window, and OpenGL renderer resources. It borrows the
 * component's display and child surface and never dispatches Wayland events.
 */
boxrig_ui_status boxrig_wayland_opengl_terminal_component_create(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out);

/**
 * Creates a configured EGL/OpenGL terminal component.
 *
 * @param context Existing Wayland context owning protocol dispatch and input.
 * @param parent Caller-owned parent wl_surface that outlives the component.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Portable component configuration copied during creation.
 * @param opengl_config EGL presentation configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE when a compatible EGL target cannot be made,
 * otherwise a portable UI status.
 */
boxrig_ui_status boxrig_wayland_opengl_terminal_component_create_with_config(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    const boxrig_wayland_opengl_component_config *opengl_config,
    boxrig_wayland_terminal_component **component_out);

/**
 * Reads adapter state without dispatching or changing the component.
 *
 * @param component OpenGL component to inspect.
 * @param diagnostic_out Receives the current adapter snapshot.
 * @return BOXRIG_UI_OK, or BOXRIG_UI_INVALID_ARGUMENT for a software component.
 */
boxrig_ui_status boxrig_wayland_opengl_terminal_component_get_diagnostic(
    const boxrig_wayland_terminal_component *component,
    boxrig_wayland_opengl_component_diagnostic *diagnostic_out);

#endif
