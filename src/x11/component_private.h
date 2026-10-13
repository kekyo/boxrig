/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_COMPONENT_PRIVATE_H
#define BOXRIG_X11_COMPONENT_PRIVATE_H

#include "boxrig/x11/component.h"

#include <fontconfig/fontconfig.h>

typedef struct boxrig_x11_component_font_presentation {
    const char *family_list;
    size_t family_list_length;
    const char *const *resource_paths;
    size_t resource_path_count;
    const boxrig_font_specification *specification;
    const boxrig_font_rendering_policy *rendering_policy;
    const boxrig_font_effective_metrics *effective_metrics;
    FcConfig *font_config;
    boxrig_text_direction_policy direction_policy;
} boxrig_x11_component_font_presentation;

typedef struct boxrig_x11_component_renderer_operations {
    int (*prepare_font)(
        void *context,
        const boxrig_x11_component_font_presentation *font);
    int (*commit_font)(void *context);
    void (*abort_font)(void *context);
    int (*set_text_direction)(
        void *context,
        boxrig_text_direction_policy policy);
    int (*apply_transparency_state)(
        void *context,
        const boxrig_ui_transparency_state *state);
    int (*set_composited_scrollbar)(
        void *context,
        bool enabled,
        unsigned int width);
    int (*set_composited_scrollbar_interaction)(
        void *context,
        bool hovered,
        bool dragging);
    int (*set_surface_size)(
        void *context,
        unsigned int width,
        unsigned int height);
    int (*set_target_extent)(
        void *context,
        unsigned int width,
        unsigned int height);
    int (*set_viewport)(
        void *context,
        int x,
        int y,
        unsigned int width,
        unsigned int height);
    int (*expose)(
        void *context,
        int x,
        int y,
        unsigned int width,
        unsigned int height);
    int (*prepare_surface_candidate)(
        void *context,
        unsigned int width,
        unsigned int height);
    int (*commit_surface_candidate)(void *context);
    void (*abort_surface_candidate)(void *context);
    int (*draw_view_composition)(
        void *context,
        boxrig_ui_view *view,
        boxrig_composition *composition);
    void (*destroy)(void *context);
} boxrig_x11_component_renderer_operations;

boxrig_ui_status boxrig_x11_terminal_component_create_adopted_child(
    Display *display,
    Window parent,
    Window child,
    Colormap owned_colormap,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    void *renderer_context,
    const boxrig_x11_component_renderer_operations *renderer_operations,
    boxrig_x11_terminal_component **component_out);

boxrig_ui_status boxrig_x11_terminal_component_create_on_target_with_renderer(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    void *renderer_context,
    const boxrig_x11_component_renderer_operations *renderer_operations,
    boxrig_x11_terminal_component **component_out);

#endif
