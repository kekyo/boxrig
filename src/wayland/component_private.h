/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WAYLAND_COMPONENT_PRIVATE_H
#define BOXRIG_WAYLAND_COMPONENT_PRIVATE_H

#include "context_private.h"
#include "../pangocairo/renderer.h"
#include "../pangocairo/renderer_private.h"
#include "shm_private.h"
#include "../ui/scrollbar_visibility.h"
#include "../ui/input_control.h"

typedef struct boxrig_wayland_font_specification_storage {
    boxrig_font_specification specification;
    boxrig_font_family *families;
    boxrig_font_feature *features;
    boxrig_font_variation *variations;
} boxrig_wayland_font_specification_storage;

typedef struct boxrig_wayland_component_font_presentation {
    const char *family_list;
    size_t family_list_length;
    const char *const *resource_paths;
    size_t resource_path_count;
    const boxrig_font_specification *specification;
    const boxrig_font_rendering_policy *rendering_policy;
    const boxrig_font_effective_metrics *effective_metrics;
    boxrig_text_direction_policy direction_policy;
} boxrig_wayland_component_font_presentation;

typedef struct boxrig_wayland_component_renderer_operations {
    int (*prepare_font)(
        void *context,
        const boxrig_wayland_component_font_presentation *font);
    int (*commit_font)(void *context);
    void (*abort_font)(void *context);
    int (*set_text_direction)(
        void *context,
        boxrig_text_direction_policy policy);
    int (*apply_transparency_state)(
        void *context,
        const boxrig_ui_transparency_state *state);
    int (*set_composited_scrollbar)(void *context, bool enabled, unsigned int width);
    int (*set_composited_scrollbar_interaction)(void *context, bool hovered, bool dragging);
    int (*prepare_surface_candidate)(
        void *context,
        unsigned int width,
        unsigned int height);
    int (*commit_surface_candidate)(void *context, bool present);
    void (*abort_surface_candidate)(void *context);
    int (*draw_view_composition)(
        void *context,
        boxrig_ui_view *view,
        boxrig_composition *composition,
        bool present);
    void (*destroy)(void *context);
} boxrig_wayland_component_renderer_operations;

struct boxrig_wayland_terminal_component {
    boxrig_wayland_context *context;
    boxrig_wayland_terminal_component *next;
    struct wl_surface *parent;
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
    struct wl_callback *frame_callback;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional_scale;
    boxrig_ui_model *model;
    boxrig_terminal *terminal;
    boxrig_ui_view *view;
    boxrig_ibus_target ibus_target;
    boxrig_composition *composition;
    boxrig_pango_cairo_renderer *renderer;
    void *presentation_renderer;
    const boxrig_wayland_component_renderer_operations
        *presentation_renderer_operations;
    boxrig_renderer_lifecycle *renderer_lifecycle;
    boxrig_renderer_target renderer_target;
    boxrig_wayland_shm_pool *pool;
    boxrig_wayland_font_specification_storage font;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_visual_step_sequence *visual_steps;
    boxrig_font_visual_step_policy visual_step_policy;
    boxrig_text_direction_policy direction_policy;
    boxrig_wayland_terminal_component_config config;
    boxrig_wayland_failure_reason failure_reason;
    uint64_t parent_state_revision;
    uint64_t effective_parent_state_revision;
    int32_t candidate_x;
    int32_t candidate_y;
    int32_t ime_window_x;
    int32_t ime_window_y;
    int32_t effective_x;
    int32_t effective_y;
    unsigned int logical_width;
    unsigned int logical_height;
    unsigned int buffer_width;
    unsigned int buffer_height;
    uint32_t scale_numerator;
    uint32_t candidate_scale_numerator;
    uint32_t preferred_buffer_scale;
    uint32_t fractional_preferred_scale;
    uint32_t *entered_outputs;
    size_t entered_output_count;
    size_t entered_output_capacity;
    uint64_t presentation_generation;
    uint64_t rendered_composition_revision;
    uint64_t scale_revision;
    int32_t buffer_scale;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_grid_resize dispatched_grid_resize;
    boxrig_ui_scrollbar_visibility_controller scrollbar_visibility;
    bool scrollbar_committing;
    bool scrollbar_hovered;
    bool scrollbar_dragging;
    boxrig_wayland_input_item *input_items;
    size_t input_queue_depth;
    size_t input_queue_head;
    size_t input_queue_high_water;
    size_t keyboard_focus_count;
    size_t pointer_focus_count;
    size_t pressed_key_count;
    size_t pressed_button_count;
    size_t input_error_count;
    uint64_t input_generation;
    /* Input inhibition is independent of clipboard ownership/focus serials. */
    boxrig_ui_input_control input_control;
    bool processing_input;
    boxrig_input_key_router key_router;
    void *key_router_context;
    boxrig_input_mouse_router mouse_router;
    void *mouse_router_context;
    bool parent_action_taken;
    bool allocation_sync_pending;
    bool layout_presentation_barrier;
    bool presentation_pending;
    bool presentation_immediate;
    bool visual_step_policy_valid;
    bool scale_dirty;
    bool scale_work_queued;
    bool allocated;
    bool visible;
    bool running;
    bool input_blocked;
    bool ime_waiting;
    bool presentation_alpha_target;
};

boxrig_ui_status boxrig_wayland_terminal_component_adopt_renderer(
    boxrig_wayland_terminal_component *component,
    void *renderer_context,
    const boxrig_wayland_component_renderer_operations *renderer_operations,
    bool alpha_target,
    const boxrig_ui_transparency_config *transparency);

boxrig_ui_status boxrig_wayland_component_scrollbar_interaction(
    boxrig_wayland_terminal_component *component, bool hovered, bool dragging);

/* Schedule the surface commit required to apply a version 2 IME rectangle. */
void boxrig_wayland_component_schedule_input_commit(
    boxrig_wayland_terminal_component *component, bool parent);

/* NULL seat cancels all seats; capture ownership remains until release. */
void boxrig_wayland_input_cancel_scrollbar(
    boxrig_wayland_terminal_component *component, boxrig_wayland_seat *seat);

#endif
