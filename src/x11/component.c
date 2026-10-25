/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/component.h"

#include "boxrig/ui/lifecycle.h"
#include "../ui/scrollbar_visibility.h"
#include "boxrig/x11/input.h"
#include "boxrig/x11/renderer.h"
#include "boxrig/x11/scrollbar.h"
#include "component_private.h"
#include "../ui/view_private.h"
#include "renderer_private.h"

#include <X11/extensions/Xrender.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BOXRIG_X11_DEFAULT_PASTE_CAPACITY (8U * 1024U * 1024U)
#define BOXRIG_X11_DEFAULT_PASTE_CHUNK_SIZE 4096U
#define BOXRIG_X11_DEFAULT_SELECTION_TRANSFER_CHUNK_SIZE (64U * 1024U)
#define BOXRIG_X11_DEFAULT_MAXIMUM_ACTIVE_SELECTION_TRANSFERS 8U
#define BOXRIG_X11_DEFAULT_MAXIMUM_SELECTION_TRANSFER_BYTES \
    (64U * 1024U * 1024U)

typedef struct boxrig_x11_font_specification_storage {
    boxrig_font_specification specification;
    boxrig_font_family *families;
    boxrig_font_feature *features;
    boxrig_font_variation *variations;
} boxrig_x11_font_specification_storage;

typedef struct boxrig_x11_component_native_operations {
    void (*set_geometry)(
        boxrig_x11_terminal_component *component,
        int x,
        int y,
        unsigned int width,
        unsigned int height);
    void (*set_visible)(
        boxrig_x11_terminal_component *component,
        bool visible);
    boxrig_ui_status (*request_focus)(
        boxrig_x11_terminal_component *component);
    void (*detach)(boxrig_x11_terminal_component *component);
    void (*destroy)(boxrig_x11_terminal_component *component);
} boxrig_x11_component_native_operations;

struct boxrig_x11_terminal_component {
    Display *display;
    Window window;
    Window input_client_window;
    Window input_window;
    boxrig_ui_component_native_hosting native_hosting;
    boxrig_ui_component_scrollbar_presentation scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_controller scrollbar_visibility;
    bool scrollbar_committing;
    const boxrig_x11_component_native_operations *native_operations;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_x11_renderer *renderer;
    void *presentation_renderer;
    const boxrig_x11_component_renderer_operations
        *presentation_renderer_operations;
    boxrig_x11_input_adapter *input;
    boxrig_x11_scrollbar *scrollbar;
    boxrig_ui_lifecycle lifecycle;
    boxrig_x11_font_specification_storage font;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_visual_step_sequence *visual_steps;
    boxrig_font_visual_step_policy visual_step_policy;
    boxrig_font_effective_metrics active_zoom_metrics;
    boxrig_font_effective_metrics pending_zoom_metrics;
    boxrig_text_direction_policy direction_policy;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_fixed transition_allocation_width;
    boxrig_ui_fixed transition_allocation_height;
    boxrig_ui_fixed surface_candidate_width;
    boxrig_ui_fixed surface_candidate_height;
    uint64_t surface_candidate_revision;
    boxrig_ui_grid_resize dispatched_grid_resize;
    bool allocation_transition_pending;
    bool surface_candidate_ready;
    bool surface_candidate_accepted;
    bool zoom_generation_active;
    bool zoom_target_pending;
    bool visual_step_policy_valid;
    bool allocated;
    bool visible;
    bool focused;
    bool composited_scrollbar_dragging;
    int composited_scrollbar_drag_anchor;
    Colormap owned_colormap;
};

static int component_renderer_prepare_font(
    boxrig_x11_terminal_component *component,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    const boxrig_font_effective_metrics *metrics,
    boxrig_text_direction_policy direction_policy)
{
    boxrig_x11_renderer_font_selection selection;
    boxrig_x11_component_font_presentation presentation;
    boxrig_font_catalog_status status;
    int result;

    if (component->presentation_renderer_operations == NULL) {
        return 0;
    }
    memset(&selection, 0, sizeof(selection));
    status = boxrig_x11_renderer_resolve_font_selection(
        component->renderer,
        specification,
        &selection);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return -1;
    }
    memset(&presentation, 0, sizeof(presentation));
    presentation.family_list = selection.family_list;
    presentation.family_list_length = strlen(selection.family_list);
    presentation.resource_paths = selection.resource_paths;
    presentation.resource_path_count = selection.resource_path_count;
    presentation.specification = specification;
    presentation.rendering_policy = policy;
    presentation.effective_metrics = metrics;
    presentation.font_config =
        boxrig_x11_renderer_font_config(component->renderer);
    presentation.direction_policy = direction_policy;
    result = component->presentation_renderer_operations->prepare_font(
        component->presentation_renderer,
        &presentation);
    boxrig_x11_renderer_font_selection_destroy(&selection);
    return result;
}

static int component_renderer_commit_font(
    boxrig_x11_terminal_component *component)
{
    return component->presentation_renderer_operations == NULL
               ? 0
               : component->presentation_renderer_operations->commit_font(
                     component->presentation_renderer);
}

static void component_renderer_abort_font(
    boxrig_x11_terminal_component *component)
{
    if (component->presentation_renderer_operations != NULL) {
        component->presentation_renderer_operations->abort_font(
            component->presentation_renderer);
    }
}

static int component_renderer_set_text_direction(
    boxrig_x11_terminal_component *component,
    boxrig_text_direction_policy policy)
{
    return component->presentation_renderer_operations == NULL
               ? 0
               : component->presentation_renderer_operations
                     ->set_text_direction(
                         component->presentation_renderer,
                         policy);
}

static int component_renderer_apply_transparency_state(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_transparency_state *state)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_apply_transparency_state(
                     component->renderer,
                     state)
               : component->presentation_renderer_operations
                     ->apply_transparency_state(
                         component->presentation_renderer,
                         state);
}

static int component_renderer_set_composited_scrollbar(
    boxrig_x11_terminal_component *component,
    bool enabled,
    unsigned int width)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_set_composited_scrollbar(
                     component->renderer,
                     enabled,
                     width)
               : component->presentation_renderer_operations
                     ->set_composited_scrollbar(
                         component->presentation_renderer,
                         enabled,
                         width);
}

static int component_renderer_set_composited_scrollbar_interaction(
    boxrig_x11_terminal_component *component,
    bool hovered,
    bool dragging)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_set_composited_scrollbar_interaction(
                     component->renderer,
                     hovered,
                     dragging)
               : component->presentation_renderer_operations
                     ->set_composited_scrollbar_interaction(
                         component->presentation_renderer,
                         hovered,
                         dragging);
}

static int component_renderer_set_surface_size(
    boxrig_x11_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_set_surface_size(
                     component->renderer,
                     width,
                     height)
               : component->presentation_renderer_operations->set_surface_size(
                     component->presentation_renderer,
                     width,
                     height);
}

static int component_renderer_set_target_extent(
    boxrig_x11_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_set_target_extent(
                     component->renderer,
                     width,
                     height)
               : component->presentation_renderer_operations->set_target_extent(
                     component->presentation_renderer,
                     width,
                     height);
}

static int component_renderer_set_viewport(
    boxrig_x11_terminal_component *component,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_set_viewport(
                     component->renderer,
                     x,
                     y,
                     width,
                     height)
               : component->presentation_renderer_operations->set_viewport(
                     component->presentation_renderer,
                     x,
                     y,
                     width,
                     height);
}

static int component_renderer_expose(
    boxrig_x11_terminal_component *component,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_expose(
                     component->renderer,
                     x,
                     y,
                     width,
                     height)
               : component->presentation_renderer_operations->expose(
                     component->presentation_renderer,
                     x,
                     y,
                     width,
                     height);
}

static int component_renderer_prepare_surface_candidate(
    boxrig_x11_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_prepare_surface_candidate(
                     component->renderer,
                     width,
                     height)
               : component->presentation_renderer_operations
                     ->prepare_surface_candidate(
                         component->presentation_renderer,
                         width,
                         height);
}

static int component_renderer_commit_surface_candidate(
    boxrig_x11_terminal_component *component)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_commit_surface_candidate(
                     component->renderer)
               : component->presentation_renderer_operations
                     ->commit_surface_candidate(
                         component->presentation_renderer);
}

static void component_renderer_abort_surface_candidate(
    boxrig_x11_terminal_component *component)
{
    if (component->presentation_renderer_operations == NULL) {
        boxrig_x11_renderer_abort_surface_candidate(component->renderer);
    } else {
        component->presentation_renderer_operations->abort_surface_candidate(
            component->presentation_renderer);
    }
}

static int component_renderer_draw_view_composition(
    boxrig_x11_terminal_component *component,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
    return component->presentation_renderer_operations == NULL
               ? boxrig_x11_renderer_draw_view_composition(
                     component->renderer,
                     view,
                     composition)
               : component->presentation_renderer_operations
                     ->draw_view_composition(
                         component->presentation_renderer,
                         view,
                         composition);
}

static void owned_child_set_geometry(
    boxrig_x11_terminal_component *component,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    XMoveResizeWindow(
        component->display,
        component->window,
        x,
        y,
        width,
        height);
}

static void owned_child_set_visible(
    boxrig_x11_terminal_component *component,
    bool visible)
{
    if (component->window == None) {
        return;
    }
    if (visible) {
        XMapWindow(component->display, component->window);
    } else {
        XUnmapWindow(component->display, component->window);
    }
}

static boxrig_ui_status owned_child_request_focus(
    boxrig_x11_terminal_component *component)
{
    XSetInputFocus(
        component->display,
        component->window,
        RevertToParent,
        CurrentTime);
    return BOXRIG_UI_OK;
}

static void owned_child_detach(boxrig_x11_terminal_component *component)
{
    if (component->window != None) {
        XSelectInput(component->display, component->window, NoEventMask);
    }
}

static void owned_child_destroy(boxrig_x11_terminal_component *component)
{
    if (component->window != None) {
        XDestroyWindow(component->display, component->window);
        component->window = None;
    }
    if (component->owned_colormap != None) {
        XFreeColormap(component->display, component->owned_colormap);
        component->owned_colormap = None;
    }
}

static const boxrig_x11_component_native_operations
    owned_child_native_operations = {
        owned_child_set_geometry,
        owned_child_set_visible,
        owned_child_request_focus,
        owned_child_detach,
        owned_child_destroy};

static void borrowed_target_set_geometry(
    boxrig_x11_terminal_component *component,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    (void)component;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void borrowed_target_set_visible(
    boxrig_x11_terminal_component *component,
    bool visible)
{
    (void)component;
    (void)visible;
}

static boxrig_ui_status borrowed_target_request_focus(
    boxrig_x11_terminal_component *component)
{
    (void)component;
    return BOXRIG_UI_UNAVAILABLE;
}

static void borrowed_target_detach(boxrig_x11_terminal_component *component)
{
    (void)component;
}

static void borrowed_target_destroy(boxrig_x11_terminal_component *component)
{
    component->window = None;
    component->input_client_window = None;
    component->input_window = None;
}

static const boxrig_x11_component_native_operations
    borrowed_target_native_operations = {
        borrowed_target_set_geometry,
        borrowed_target_set_visible,
        borrowed_target_request_focus,
        borrowed_target_detach,
        borrowed_target_destroy};

static bool valid_attachment(boxrig_ui_component_attachment attachment)
{
    return attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY ||
           attachment == BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
}

static bool valid_scrollbar_presentation(
    boxrig_ui_component_scrollbar_presentation presentation)
{
    return presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_NONE ||
           presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD ||
           presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
}

static bool valid_presentation_renderer(
    void *context,
    const boxrig_x11_component_renderer_operations *operations)
{
    if (context == NULL || operations == NULL) {
        return context == NULL && operations == NULL;
    }
    return operations->prepare_font != NULL &&
           operations->commit_font != NULL && operations->abort_font != NULL &&
           operations->set_text_direction != NULL &&
           operations->apply_transparency_state != NULL &&
           operations->set_composited_scrollbar != NULL &&
           operations->set_composited_scrollbar_interaction != NULL &&
           operations->set_surface_size != NULL &&
           operations->set_target_extent != NULL &&
           operations->set_viewport != NULL && operations->expose != NULL &&
           operations->prepare_surface_candidate != NULL &&
           operations->commit_surface_candidate != NULL &&
           operations->abort_surface_candidate != NULL &&
           operations->draw_view_composition != NULL &&
           operations->destroy != NULL;
}

static void font_specification_storage_destroy(
    boxrig_x11_font_specification_storage *storage)
{
    size_t index;

    if (storage == NULL) {
        return;
    }
    if (storage->families != NULL) {
        for (index = 0; index < storage->specification.family_count; index++) {
            free((void *)storage->families[index].name);
        }
    }
    free(storage->families);
    free(storage->features);
    free(storage->variations);
    memset(storage, 0, sizeof(*storage));
}

static bool font_specification_storage_copy(
    const boxrig_font_specification *specification,
    boxrig_x11_font_specification_storage *storage_out)
{
    boxrig_x11_font_specification_storage storage;
    size_t index;

    if (specification == NULL || storage_out == NULL ||
        (specification->family_count != 0 &&
         specification->families == NULL) ||
        (specification->feature_count != 0 &&
         specification->features == NULL) ||
        (specification->variation_count != 0 &&
         specification->variations == NULL) ||
        specification->family_count > SIZE_MAX / sizeof(*storage.families) ||
        specification->feature_count > SIZE_MAX / sizeof(*storage.features) ||
        specification->variation_count >
            SIZE_MAX / sizeof(*storage.variations)) {
        return false;
    }
    memset(&storage, 0, sizeof(storage));
    storage.specification = *specification;
    if (specification->family_count != 0) {
        storage.families = calloc(
            specification->family_count,
            sizeof(*storage.families));
        if (storage.families == NULL) {
            goto failure;
        }
        storage.specification.families = storage.families;
        for (index = 0; index < specification->family_count; index++) {
            const boxrig_font_family *source = &specification->families[index];
            boxrig_font_family *destination = &storage.families[index];

            *destination = *source;
            if (source->name != NULL) {
                char *name;

                if (source->name_length == SIZE_MAX) {
                    goto failure;
                }
                name = malloc(source->name_length + 1);
                if (name == NULL) {
                    goto failure;
                }
                memcpy(name, source->name, source->name_length);
                name[source->name_length] = '\0';
                destination->name = name;
            }
        }
    }
    if (specification->feature_count != 0) {
        storage.features = malloc(
            specification->feature_count * sizeof(*storage.features));
        if (storage.features == NULL) {
            goto failure;
        }
        memcpy(
            storage.features,
            specification->features,
            specification->feature_count * sizeof(*storage.features));
        storage.specification.features = storage.features;
    }
    if (specification->variation_count != 0) {
        storage.variations = malloc(
            specification->variation_count * sizeof(*storage.variations));
        if (storage.variations == NULL) {
            goto failure;
        }
        memcpy(
            storage.variations,
            specification->variations,
            specification->variation_count * sizeof(*storage.variations));
        storage.specification.variations = storage.variations;
    }
    *storage_out = storage;
    return true;

failure:
    font_specification_storage_destroy(&storage);
    return false;
}

static boxrig_ui_status status_from_font_catalog(
    boxrig_font_catalog_status status)
{
    switch (status) {
    case BOXRIG_FONT_CATALOG_OK:
        return BOXRIG_UI_OK;
    case BOXRIG_FONT_CATALOG_OUT_OF_MEMORY:
        return BOXRIG_UI_OUT_OF_MEMORY;
    case BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED:
        return BOXRIG_UI_LIMIT_EXCEEDED;
    case BOXRIG_FONT_CATALOG_INVALID_ARGUMENT:
    case BOXRIG_FONT_CATALOG_IO_ERROR:
    case BOXRIG_FONT_CATALOG_INVALID_FONT:
    case BOXRIG_FONT_CATALOG_NOT_FOUND:
    default:
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
}

static bool event_targets_window(const XEvent *event, Window window)
{
    switch (event->type) {
    case SelectionNotify:
        return event->xselection.requestor == window;
    case SelectionRequest:
        return event->xselectionrequest.owner == window;
    case SelectionClear:
        return event->xselectionclear.window == window;
    default:
        return event->xany.window == window;
    }
}

static unsigned int component_scrollbar_width(const boxrig_x11_terminal_component *component)
{
    return component->scrollbar_committing
        ? component->scrollbar_visibility.target_width
        : component->scrollbar_visibility.state.reserved_width;
}

static unsigned int component_content_width(
    const boxrig_x11_terminal_component *component,
    unsigned int width)
{
    unsigned int scrollbar_width = component_scrollbar_width(component);

    if (width <= scrollbar_width) {
        return 1;
    }
    return width - scrollbar_width;
}

static bool composited_scrollbar_pointer_position(
    const boxrig_x11_terminal_component *component,
    const XEvent *event,
    int64_t *x_out,
    int64_t *y_out)
{
    int event_x;
    int event_y;
    int origin_x;
    int origin_y;

    if (event->type == ButtonPress || event->type == ButtonRelease) {
        event_x = event->xbutton.x;
        event_y = event->xbutton.y;
    } else if (event->type == MotionNotify) {
        event_x = event->xmotion.x;
        event_y = event->xmotion.y;
    } else {
        return false;
    }
    origin_x = component->native_hosting ==
                       BOXRIG_UI_COMPONENT_BORROWED_TARGET
                   ? component->allocation.x
                   : 0;
    origin_y = component->native_hosting ==
                       BOXRIG_UI_COMPONENT_BORROWED_TARGET
                   ? component->allocation.y
                   : 0;
    *x_out = (int64_t)event_x - origin_x;
    *y_out = (int64_t)event_y - origin_y;
    return true;
}

static int apply_composited_scrollbar_action(
    boxrig_x11_terminal_component *component,
    boxrig_scrollbar_action action,
    size_t position)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(component->model);
    bool changed;

    return terminal != NULL &&
                   boxrig_scrollbar_apply_action(
                       terminal,
                       action,
                       position,
                       &changed) == 0
               ? 0
               : -1;
}

static int process_composited_scrollbar_event(
    boxrig_x11_terminal_component *component,
    const XEvent *event)
{
    boxrig_terminal *terminal;
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    unsigned int width;
    int64_t x;
    int64_t y;
    int64_t left;
    bool inside;

    if (component->scrollbar_presentation !=
            BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED ||
        !component->scrollbar_visibility.state.visible ||
        !component->allocated ||
        !composited_scrollbar_pointer_position(
            component,
            event,
            &x,
            &y)) {
        return 0;
    }
    width = component_scrollbar_width(component);
    if (width > component->allocation.width) {
        width = component->allocation.width;
    }
    left = (int64_t)component->allocation.width - width;
    inside = x >= left && x < component->allocation.width && y >= 0 &&
             y < component->allocation.height;
    if (event->type == MotionNotify) {
        if (component->composited_scrollbar_dragging) {
            int64_t requested = y -
                                component->composited_scrollbar_drag_anchor;
            size_t thumb_start = requested < 0 ? 0 : (size_t)requested;
            size_t position;

            terminal = boxrig_ui_model_terminal(component->model);
            if (terminal == NULL ||
                boxrig_scrollbar_model_read(terminal, &model) == -1) {
                return -1;
            }
            position = boxrig_scrollbar_position_from_thumb(
                &model,
                component->allocation.height,
                width,
                thumb_start);
            if (apply_composited_scrollbar_action(
                    component,
                    BOXRIG_SCROLLBAR_POSITION,
                    position) == -1 ||
                component_renderer_set_composited_scrollbar_interaction(
                    component,
                    inside,
                    true) == -1) {
                return -1;
            }
            return 1;
        }
        if (component_renderer_set_composited_scrollbar_interaction(
                component,
                inside,
                false) == -1) {
            return -1;
        }
        return inside ? 1 : 0;
    }
    if (event->type == ButtonRelease) {
        bool was_dragging = component->composited_scrollbar_dragging;

        if (event->xbutton.button == Button1) {
            component->composited_scrollbar_dragging = false;
        }
        if (component_renderer_set_composited_scrollbar_interaction(
                component,
                inside,
                component->composited_scrollbar_dragging) == -1) {
            return -1;
        }
        return inside || was_dragging ? 1 : 0;
    }
    if (!inside) {
        return 0;
    }
    terminal = boxrig_ui_model_terminal(component->model);
    if (terminal == NULL ||
        boxrig_scrollbar_model_read(terminal, &model) == -1) {
        return -1;
    }
    boxrig_scrollbar_calculate_thumb(
        &model,
        component->allocation.height,
        width,
        &thumb);
    if (event->xbutton.button == Button1 && model.enabled) {
        size_t clamped_y = (size_t)y;

        if (clamped_y >= thumb.start &&
            clamped_y < thumb.start + thumb.length) {
            component->composited_scrollbar_dragging = true;
            component->composited_scrollbar_drag_anchor =
                (int)(y - (int64_t)thumb.start);
        } else if (clamped_y < thumb.start) {
            if (apply_composited_scrollbar_action(
                    component,
                    BOXRIG_SCROLLBAR_PAGE_UP,
                    0) == -1) {
                return -1;
            }
        } else if (apply_composited_scrollbar_action(
                       component,
                       BOXRIG_SCROLLBAR_PAGE_DOWN,
                       0) == -1) {
            return -1;
        }
    } else if (event->xbutton.button == Button4) {
        if (apply_composited_scrollbar_action(
                component,
                BOXRIG_SCROLLBAR_LINE_UP,
                0) == -1) {
            return -1;
        }
    } else if (event->xbutton.button == Button5 &&
               apply_composited_scrollbar_action(
                   component,
                   BOXRIG_SCROLLBAR_LINE_DOWN,
                   0) == -1) {
        return -1;
    }
    return component_renderer_set_composited_scrollbar_interaction(
               component,
               true,
               component->composited_scrollbar_dragging) == -1
               ? -1
               : 1;
}

static void clear_surface_candidate_state(
    boxrig_x11_terminal_component *component)
{
    component->surface_candidate_width = 0;
    component->surface_candidate_height = 0;
    component->surface_candidate_revision = 0;
    component->surface_candidate_ready = false;
    component->surface_candidate_accepted = false;
}

static boxrig_ui_status consume_presentation_dirty(
    boxrig_ui_view *view)
{
    boxrig_ui_dirty dirty;

    while (boxrig_ui_view_peek_dirty(
               view,
               BOXRIG_UI_DIRTY_PRESENT,
               &dirty)) {
        if (boxrig_ui_view_consume_dirty(
                view,
                BOXRIG_UI_DIRTY_PRESENT) != BOXRIG_UI_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    return BOXRIG_UI_OK;
}

static bool request_pixel_size(
    boxrig_ui_fixed value,
    unsigned int *pixels_out)
{
    uint64_t pixels;

    if (value <= 0 || value % BOXRIG_UI_FIXED_ONE != 0 ||
        pixels_out == NULL) {
        return false;
    }
    pixels = (uint64_t)(value / BOXRIG_UI_FIXED_ONE);
    if (pixels == 0 || pixels > UINT_MAX) {
        return false;
    }
    *pixels_out = (unsigned int)pixels;
    return true;
}

static bool presentation_waits_for_allocation(
    const boxrig_x11_terminal_component *component)
{
    boxrig_ui_size_command_state state;

    return component->allocation_transition_pending ||
           (boxrig_ui_view_get_size_command_state(
                component->view,
                &state) == BOXRIG_UI_OK &&
            state != BOXRIG_UI_SIZE_COMMAND_IDLE);
}

static bool surface_candidate_preparation_pending(
    const boxrig_x11_terminal_component *component)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(component->model);
    boxrig_ui_layout_action action;

    return terminal != NULL &&
           !boxrig_terminal_synchronized_output(terminal) &&
           component->zoom_generation_active &&
           !component->surface_candidate_ready &&
           boxrig_ui_view_peek_layout_action(component->view, &action) &&
           action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE &&
           action.value.content_size.cause ==
               BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
}

static bool size_command_ready_for_dispatch(
    const boxrig_x11_terminal_component *component)
{
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state state;

    return boxrig_ui_view_get_size_command_state(
               component->view, &state) == BOXRIG_UI_OK &&
           state == BOXRIG_UI_SIZE_COMMAND_READY &&
           boxrig_ui_view_peek_layout_action(component->view, &action) &&
           action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE &&
           (!component->zoom_generation_active ||
            action.value.content_size.cause !=
                BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS ||
            component->surface_candidate_ready);
}

static bool grid_resize_waiting_for_application(
    const boxrig_x11_terminal_component *component,
    bool *ready_out)
{
    boxrig_ui_layout_action action;

    *ready_out = false;
    if (!component->zoom_generation_active && !component->scrollbar_visibility.state.pending) {
        return false;
    }
    if (component->dispatched_grid_resize.revision != 0) {
        return true;
    }
    if (boxrig_ui_view_peek_layout_action(component->view, &action) &&
        action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        *ready_out = true;
        return true;
    }
    return false;
}

static boxrig_ui_status prepare_zoom_surface_candidate(
    boxrig_x11_terminal_component *component)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(component->model);
    boxrig_ui_layout_action action;
    boxrig_ui_layout_result layout;
    unsigned int content_width;
    unsigned int height;
    unsigned int scrollbar_width = component_scrollbar_width(component);

    if (terminal == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_terminal_synchronized_output(terminal) ||
        !component->zoom_generation_active ||
        !boxrig_ui_view_peek_layout_action(component->view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE ||
        action.value.content_size.cause !=
            BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS) {
        return BOXRIG_UI_OK;
    }
    if (boxrig_ui_view_get_layout(component->view, &layout) !=
        BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->surface_candidate_ready) {
        return component->surface_candidate_width == layout.grid_width &&
                       component->surface_candidate_height == layout.grid_height
                   ? BOXRIG_UI_OK
                   : BOXRIG_UI_UNAVAILABLE;
    }
    if (!request_pixel_size(layout.grid_width, &content_width) ||
        !request_pixel_size(layout.grid_height, &height) ||
        content_width > UINT_MAX - scrollbar_width ||
        component_renderer_prepare_surface_candidate(
            component,
            content_width + scrollbar_width,
            height) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component_renderer_draw_view_composition(
            component,
            component->view,
            boxrig_x11_input_adapter_composition(component->input)) == -1) {
        component_renderer_abort_surface_candidate(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    component->surface_candidate_width = layout.grid_width;
    component->surface_candidate_height = layout.grid_height;
    component->surface_candidate_revision = 0;
    component->surface_candidate_ready = true;
    component->surface_candidate_accepted = false;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status configure_transparency(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_transparency_config *config)
{
    XWindowAttributes attributes;
    XRenderPictFormat *format;
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state state;
    boxrig_ui_status status;

    if (XGetWindowAttributes(
            component->display,
            component->window,
            &attributes) == 0) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    format = XRenderFindVisualFormat(component->display, attributes.visual);
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target =
        attributes.depth == 32 && format != NULL && format->depth == 32 &&
        format->type == PictTypeDirect && format->direct.alphaMask != 0;
    if (capabilities.premultiplied_argb32_target) {
        capabilities.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    status = boxrig_ui_view_report_transparency_capabilities(
        component->view,
        &capabilities);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_ui_view_set_transparency_config(component->view, config);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_ui_view_get_transparency(component->view, &state);
    if (status != BOXRIG_UI_OK ||
        component_renderer_apply_transparency_state(
            component,
            &state) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

boxrig_x11_terminal_component_config
boxrig_x11_terminal_component_config_default(void)
{
    boxrig_x11_terminal_component_config config;

    config.view = boxrig_ui_view_config_default();
    config.animation_runtime = NULL;
    config.attachment = BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD;
    config.paste_capacity = BOXRIG_X11_DEFAULT_PASTE_CAPACITY;
    config.paste_chunk_size = BOXRIG_X11_DEFAULT_PASTE_CHUNK_SIZE;
    config.selection_transfer_chunk_size =
        BOXRIG_X11_DEFAULT_SELECTION_TRANSFER_CHUNK_SIZE;
    config.maximum_active_selection_transfers =
        BOXRIG_X11_DEFAULT_MAXIMUM_ACTIVE_SELECTION_TRANSFERS;
    config.maximum_selection_transfer_bytes =
        BOXRIG_X11_DEFAULT_MAXIMUM_SELECTION_TRANSFER_BYTES;
    config.transparency = boxrig_ui_transparency_config_default();
    return config;
}

static boxrig_ui_status initialize_component_resources(
    boxrig_x11_terminal_component *component,
    const boxrig_x11_terminal_component_config *config)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(component->model);
    boxrig_font_effective_metrics metrics;
    boxrig_font_specification default_specification;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (config->animation_runtime == NULL &&
        (config->view.blink.text_enabled || config->view.blink.cursor_enabled)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (terminal == NULL ||
        boxrig_x11_renderer_create(
            component->display,
            component->window,
            &component->renderer) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    font_status = boxrig_x11_renderer_get_effective_metrics(
        component->renderer,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return status_from_font_catalog(font_status);
    }
    status = boxrig_ui_view_create(
        component->model,
        &config->view,
        &component->view);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (config->animation_runtime != NULL) {
        status = boxrig_ui_animation_runtime_attach(config->animation_runtime, component->view);
        if (status != BOXRIG_UI_OK) return status;
    }
    status = boxrig_ui_view_require_animation_runtime(component->view);
    if (status != BOXRIG_UI_OK) return status;
    if (config->attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
        status = boxrig_ui_view_acquire_layout_authority(component->view);
        if (status == BOXRIG_UI_OK) {
            status = boxrig_ui_view_commit_cell_metrics(
                component->view,
                &metrics);
        }
    } else {
        status = boxrig_ui_view_validate_cell_metrics(
            component->view,
            &metrics);
    }
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_x11_renderer_bind_view(component->renderer, component->view);
    if (status != BOXRIG_UI_OK) return status;
    status = boxrig_ui_view_set_geometry_capabilities(
        component->view,
        BOXRIG_UI_GEOMETRY_SIZE_COMMAND |
            BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = configure_transparency(component, &config->transparency);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (component_renderer_set_composited_scrollbar(
            component,
            config->scrollbar_presentation ==
                BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED,
            boxrig_x11_scrollbar_pixel_width()) == -1 ||
        boxrig_x11_input_adapter_create(
            component->display,
            component->input_client_window,
            component->input_window,
            terminal,
            metrics.cell_width,
            metrics.cell_height,
            config->paste_capacity,
            config->paste_chunk_size,
            &component->input) == -1 ||
        boxrig_x11_input_adapter_set_editable_document(
            component->input,
            boxrig_ui_view_editable_document(component->view)) == -1 ||
        boxrig_x11_input_adapter_set_clipboard_transfer_limits(
            component->input,
            config->selection_transfer_chunk_size,
            config->maximum_active_selection_transfers,
            config->maximum_selection_transfer_bytes) == -1 ||
        (config->scrollbar_presentation ==
             BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD &&
         boxrig_x11_scrollbar_create(
             component->display,
             component->window,
             terminal,
             &component->scrollbar) == -1) ||
        (component->scrollbar != NULL &&
         boxrig_x11_scrollbar_resize(component->scrollbar, 1, 1) == -1) ||
        boxrig_ui_view_set_visible(component->view, false) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    default_specification = boxrig_font_specification_default();
    if (!font_specification_storage_copy(
            &default_specification,
            &component->font)) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->rendering_policy = boxrig_font_rendering_policy_default();
    component->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    if (component_renderer_prepare_font(
            component,
            &component->font.specification,
            &component->rendering_policy,
            &metrics,
            component->direction_policy) == -1 ||
        component_renderer_commit_font(component) == -1) {
        component_renderer_abort_font(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

static bool borrowed_targets_compatible(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    XWindowAttributes *presentation_attributes_out)
{
    XWindowAttributes presentation_attributes;
    XWindowAttributes input_attributes;

    if (XGetWindowAttributes(
            display,
            target->presentation_window,
            &presentation_attributes) == 0 ||
        XGetWindowAttributes(
            display,
            target->input_window,
            &input_attributes) == 0 ||
        presentation_attributes.class != InputOutput ||
        input_attributes.class != InputOutput ||
        presentation_attributes.root != input_attributes.root ||
        presentation_attributes.screen != input_attributes.screen ||
        presentation_attributes.visual != input_attributes.visual ||
        presentation_attributes.depth != input_attributes.depth ||
        presentation_attributes.colormap != input_attributes.colormap) {
        return false;
    }
    *presentation_attributes_out = presentation_attributes;
    return true;
}

boxrig_ui_status boxrig_x11_terminal_component_create_child(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_x11_terminal_component *component;
    boxrig_ui_status status;
    XSetWindowAttributes attributes;

    if (display == NULL || parent == None || model == NULL || config == NULL ||
        component_out == NULL || !valid_attachment(config->attachment) ||
        !valid_scrollbar_presentation(config->scrollbar_presentation) ||
        config->paste_chunk_size == 0 ||
        config->selection_transfer_chunk_size == 0 ||
        config->maximum_active_selection_transfers == 0 ||
        config->maximum_selection_transfer_bytes == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (boxrig_ui_model_terminal(model) == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->display = display;
    component->model = model;
    component->native_hosting = BOXRIG_UI_COMPONENT_OWNED_CHILD;
    component->scrollbar_presentation = config->scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_initialize(&component->scrollbar_visibility,
        config->scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE, boxrig_x11_scrollbar_pixel_width());
    component->native_operations = &owned_child_native_operations;
    boxrig_ui_lifecycle_initialize(&component->lifecycle);
    memset(&attributes, 0, sizeof(attributes));
    attributes.event_mask =
        ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
        ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
        PropertyChangeMask | StructureNotifyMask;
    component->window = XCreateWindow(
        display,
        parent,
        0,
        0,
        1,
        1,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWEventMask,
        &attributes);
    component->input_client_window = parent;
    component->input_window = component->window;
    if (component->window == None) {
        boxrig_x11_terminal_component_destroy(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = initialize_component_resources(component, config);
    if (status != BOXRIG_UI_OK) {
        boxrig_x11_terminal_component_destroy(component);
        return status;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_create_adopted_child(
    Display *display,
    Window parent,
    Window child,
    Colormap owned_colormap,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    void *renderer_context,
    const boxrig_x11_component_renderer_operations *renderer_operations,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_x11_terminal_component *component;
    XWindowAttributes parent_attributes;
    XWindowAttributes child_attributes;
    Window root;
    Window actual_parent;
    Window *children = NULL;
    unsigned int child_count;
    boxrig_ui_status status;

    if (display == NULL || parent == None || child == None ||
        owned_colormap == None || model == NULL || config == NULL ||
        component_out == NULL || !valid_attachment(config->attachment) ||
        !valid_scrollbar_presentation(config->scrollbar_presentation) ||
        config->paste_chunk_size == 0 ||
        config->selection_transfer_chunk_size == 0 ||
        config->maximum_active_selection_transfers == 0 ||
        config->maximum_selection_transfer_bytes == 0 ||
        renderer_context == NULL ||
        !valid_presentation_renderer(
            renderer_context,
            renderer_operations)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (boxrig_ui_model_terminal(model) == NULL) {
        renderer_operations->destroy(renderer_context);
        XDestroyWindow(display, child);
        XFreeColormap(display, owned_colormap);
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (XGetWindowAttributes(display, parent, &parent_attributes) == 0 ||
        XGetWindowAttributes(display, child, &child_attributes) == 0 ||
        XQueryTree(
            display,
            child,
            &root,
            &actual_parent,
            &children,
            &child_count) == 0) {
        if (children != NULL) {
            XFree(children);
        }
        renderer_operations->destroy(renderer_context);
        XDestroyWindow(display, child);
        XFreeColormap(display, owned_colormap);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (children != NULL) {
        XFree(children);
    }
    if (actual_parent != parent || child_attributes.class != InputOutput ||
        child_attributes.root != parent_attributes.root ||
        child_attributes.screen != parent_attributes.screen ||
        child_attributes.colormap != owned_colormap) {
        renderer_operations->destroy(renderer_context);
        XDestroyWindow(display, child);
        XFreeColormap(display, owned_colormap);
        return BOXRIG_UI_UNAVAILABLE;
    }
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        renderer_operations->destroy(renderer_context);
        XDestroyWindow(display, child);
        XFreeColormap(display, owned_colormap);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->display = display;
    component->window = child;
    component->input_client_window = parent;
    component->input_window = child;
    component->model = model;
    component->native_hosting = BOXRIG_UI_COMPONENT_OWNED_CHILD;
    component->scrollbar_presentation = config->scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_initialize(&component->scrollbar_visibility,
        config->scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE, boxrig_x11_scrollbar_pixel_width());
    component->native_operations = &owned_child_native_operations;
    component->presentation_renderer = renderer_context;
    component->presentation_renderer_operations = renderer_operations;
    component->owned_colormap = owned_colormap;
    boxrig_ui_lifecycle_initialize(&component->lifecycle);
    status = initialize_component_resources(component, config);
    if (status != BOXRIG_UI_OK) {
        boxrig_x11_terminal_component_destroy(component);
        return status;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_create_on_target_with_renderer(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    void *renderer_context,
    const boxrig_x11_component_renderer_operations *renderer_operations,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_x11_terminal_component *component;
    XWindowAttributes presentation_attributes;
    boxrig_ui_status status;

    if (display == NULL || target == NULL ||
        target->presentation_window == None || target->input_window == None ||
        model == NULL || config == NULL || component_out == NULL ||
        !valid_attachment(config->attachment) ||
        !valid_scrollbar_presentation(config->scrollbar_presentation) ||
        config->scrollbar_presentation ==
            BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD ||
        config->paste_chunk_size == 0 ||
        config->selection_transfer_chunk_size == 0 ||
        config->maximum_active_selection_transfers == 0 ||
        config->maximum_selection_transfer_bytes == 0 ||
        !valid_presentation_renderer(
            renderer_context,
            renderer_operations)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (boxrig_ui_model_terminal(model) == NULL) {
        if (renderer_operations != NULL) {
            renderer_operations->destroy(renderer_context);
        }
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!borrowed_targets_compatible(
            display,
            target,
            &presentation_attributes)) {
        if (renderer_operations != NULL) {
            renderer_operations->destroy(renderer_context);
        }
        return BOXRIG_UI_UNAVAILABLE;
    }
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        if (renderer_operations != NULL) {
            renderer_operations->destroy(renderer_context);
        }
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->display = display;
    component->window = target->presentation_window;
    component->input_client_window = target->input_window;
    component->input_window = target->input_window;
    component->model = model;
    component->native_hosting = BOXRIG_UI_COMPONENT_BORROWED_TARGET;
    component->scrollbar_presentation = config->scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_initialize(&component->scrollbar_visibility,
        config->scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE, boxrig_x11_scrollbar_pixel_width());
    component->native_operations = &borrowed_target_native_operations;
    component->presentation_renderer = renderer_context;
    component->presentation_renderer_operations = renderer_operations;
    boxrig_ui_lifecycle_initialize(&component->lifecycle);
    status = initialize_component_resources(component, config);
    if (status == BOXRIG_UI_OK &&
        component_renderer_set_target_extent(
            component,
            (unsigned int)presentation_attributes.width,
            (unsigned int)presentation_attributes.height) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
    }
    if (status != BOXRIG_UI_OK) {
        boxrig_x11_terminal_component_destroy(component);
        return status;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_create_on_target(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out)
{
    return boxrig_x11_terminal_component_create_on_target_with_renderer(
        display,
        target,
        model,
        config,
        NULL,
        NULL,
        component_out);
}

boxrig_ui_status boxrig_x11_terminal_component_quiesce(
    boxrig_x11_terminal_component *component)
{
    boxrig_ui_lifecycle_state state;

    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    state = boxrig_ui_lifecycle_get_state(&component->lifecycle);
    if (state == BOXRIG_UI_LIFECYCLE_RUNNING &&
        boxrig_ui_lifecycle_begin_shutdown(&component->lifecycle) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->view != NULL &&
        boxrig_ui_view_set_visible(component->view, false) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->view != NULL &&
        boxrig_ui_view_set_focused(component->view, false) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->input != NULL &&
        boxrig_x11_input_adapter_quiesce(component->input) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_scrollbar_visibility_cancel(&component->scrollbar_visibility);
    boxrig_ui_animation_runtime_detach(component->view);
    component->visible = false;
    component->focused = false;
    memset(
        &component->dispatched_grid_resize,
        0,
        sizeof(component->dispatched_grid_resize));
    if (component->native_operations != NULL) {
        component->native_operations->set_visible(component, false);
    }
    return BOXRIG_UI_OK;
}

void boxrig_x11_terminal_component_destroy(
    boxrig_x11_terminal_component *component)
{
    if (component == NULL) {
        return;
    }
    (void)boxrig_x11_terminal_component_quiesce(component);
    if (component->native_operations != NULL) {
        component->native_operations->detach(component);
    }
    (void)boxrig_ui_lifecycle_mark_detached(&component->lifecycle);
    boxrig_x11_scrollbar_destroy(component->scrollbar);
    boxrig_x11_input_adapter_destroy(component->input);
    if (component->presentation_renderer_operations != NULL) {
        component->presentation_renderer_operations->destroy(
            component->presentation_renderer);
        component->presentation_renderer = NULL;
        component->presentation_renderer_operations = NULL;
    }
    boxrig_x11_renderer_destroy(component->renderer);
    boxrig_ui_view_destroy(component->view);
    boxrig_font_visual_step_sequence_destroy(component->visual_steps);
    font_specification_storage_destroy(&component->font);
    if (component->native_operations != NULL) {
        component->native_operations->destroy(component);
    }
    (void)boxrig_ui_lifecycle_mark_stopped(&component->lifecycle);
    free(component);
}

boxrig_ui_component_native_hosting
boxrig_x11_terminal_component_native_hosting(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL ? BOXRIG_UI_COMPONENT_OWNED_CHILD
                             : component->native_hosting;
}

Window boxrig_x11_terminal_component_owned_window(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL ||
                   component->native_hosting != BOXRIG_UI_COMPONENT_OWNED_CHILD
               ? None
               : component->window;
}

Window boxrig_x11_terminal_component_presentation_window(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL ? None : component->window;
}

Window boxrig_x11_terminal_component_input_window(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL ? None : component->input_window;
}

bool boxrig_x11_terminal_component_input_window_owned(
    const boxrig_x11_terminal_component *component)
{
    return component != NULL && component->input_window != None &&
           component->native_hosting == BOXRIG_UI_COMPONENT_OWNED_CHILD;
}

Window boxrig_x11_terminal_component_scrollbar_window(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL
               ? None
               : boxrig_x11_scrollbar_window(component->scrollbar);
}

boxrig_ui_status boxrig_x11_terminal_component_set_target_extent(
    boxrig_x11_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    if (component == NULL || width == 0 || height == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->native_hosting != BOXRIG_UI_COMPONENT_BORROWED_TARGET) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return component_renderer_set_target_extent(
               component,
               width,
               height) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_UNAVAILABLE;
}

boxrig_ui_view *boxrig_x11_terminal_component_view(
    boxrig_x11_terminal_component *component)
{
    return component == NULL ? NULL : component->view;
}

boxrig_composition *boxrig_x11_terminal_component_composition(
    boxrig_x11_terminal_component *component)
{
    return component == NULL
               ? NULL
               : boxrig_x11_input_adapter_composition(component->input);
}

boxrig_ui_model *boxrig_x11_terminal_component_model(
    boxrig_x11_terminal_component *component)
{
    return component == NULL ? NULL : component->model;
}

boxrig_ui_status boxrig_x11_terminal_component_allocate(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_status status;
    unsigned int content_width;
    int viewport_x;
    int viewport_y;
    bool preserve_candidate;
    bool commit_candidate;
    bool resolve_candidate;
    bool completion_echo;

    if (component == NULL || allocation == NULL || layout_out == NULL ||
        allocation->width == 0 || allocation->height == 0 ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle) ||
        boxrig_x11_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->scrollbar_visibility.state.pending && !component->scrollbar_committing)
        return BOXRIG_UI_BUSY;
    content_width = component_content_width(component, allocation->width);
    memset(&input, 0, sizeof(input));
    input.allocation_width =
        (boxrig_ui_fixed)content_width * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)allocation->height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = allocation->window_state;
    completion_echo =
        component->allocation_transition_pending &&
        input.allocation_width == component->transition_allocation_width &&
        input.allocation_height == component->transition_allocation_height;
    status = completion_echo
                 ? boxrig_ui_view_get_layout(component->view, layout_out)
                 : boxrig_ui_view_accept_allocation(
                       component->view,
                       &input,
                       layout_out);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    preserve_candidate = component->surface_candidate_ready &&
                         !component->allocation_transition_pending &&
                         component->allocated &&
                         allocation->width == component->allocation.width &&
                         allocation->height == component->allocation.height;
    resolve_candidate = component->surface_candidate_ready &&
                        !preserve_candidate;
    commit_candidate = resolve_candidate &&
                       component->allocation_transition_pending &&
                       component->surface_candidate_accepted &&
                       input.allocation_width ==
                           component->surface_candidate_width &&
                       input.allocation_height ==
                           component->surface_candidate_height;
    if (!component->allocated ||
        allocation->x != component->allocation.x ||
        allocation->y != component->allocation.y ||
        allocation->width != component->allocation.width ||
        allocation->height != component->allocation.height) {
        component->native_operations->set_geometry(
            component,
            allocation->x,
            allocation->y,
            allocation->width,
            allocation->height);
    }
    viewport_x = component->native_hosting ==
                         BOXRIG_UI_COMPONENT_BORROWED_TARGET
                     ? allocation->x
                     : 0;
    viewport_y = component->native_hosting ==
                         BOXRIG_UI_COMPONENT_BORROWED_TARGET
                     ? allocation->y
                     : 0;
    if ((component->native_hosting == BOXRIG_UI_COMPONENT_OWNED_CHILD &&
         component_renderer_set_target_extent(
             component,
             allocation->width,
             allocation->height) == -1) ||
        component_renderer_set_viewport(
            component,
            viewport_x,
            viewport_y,
            allocation->width,
            allocation->height) == -1 ||
        boxrig_x11_input_adapter_set_viewport_origin(
            component->input,
            viewport_x,
            viewport_y) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if ((commit_candidate &&
         component_renderer_commit_surface_candidate(component) == -1)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (resolve_candidate && !commit_candidate) {
        component_renderer_abort_surface_candidate(component);
    }
    if (resolve_candidate) {
        clear_surface_candidate_state(component);
    }
    if ((!preserve_candidate && !commit_candidate &&
         component_renderer_set_surface_size(
             component,
             allocation->width,
             allocation->height) == -1) ||
        boxrig_x11_input_adapter_set_cell_size(
            component->input,
            metrics.cell_width,
            metrics.cell_height) == -1 ||
        (component->scrollbar != NULL &&
         boxrig_x11_scrollbar_resize(
             component->scrollbar,
             allocation->width,
             allocation->height) == -1)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (commit_candidate &&
        consume_presentation_dirty(component->view) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    component->allocation = *allocation;
    component->allocated = true;
    if (component->allocation_transition_pending &&
        (boxrig_ui_fixed)content_width * BOXRIG_UI_FIXED_ONE ==
            component->transition_allocation_width &&
        (boxrig_ui_fixed)allocation->height * BOXRIG_UI_FIXED_ONE ==
            component->transition_allocation_height) {
        component->allocation_transition_pending = false;
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status commit_scrollbar_visibility(boxrig_x11_terminal_component *component)
{
    if (!component->scrollbar_visibility.state.pending) return BOXRIG_UI_OK;
    bool show = component->scrollbar_visibility.target_visible;
    bool composited = component->scrollbar_presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    if (component_renderer_set_composited_scrollbar(component, show && composited, boxrig_x11_scrollbar_pixel_width()) == -1)
        return BOXRIG_UI_UNAVAILABLE;
    component->scrollbar_committing = true;
    boxrig_ui_status status = BOXRIG_UI_OK;
    if (component->allocated) {
        boxrig_ui_layout_result layout;
        status = boxrig_x11_terminal_component_allocate(component, &component->allocation, &layout);
    }
    component->scrollbar_committing = false;
    if (status != BOXRIG_UI_OK) {
        (void)component_renderer_set_composited_scrollbar(component,
            component->scrollbar_visibility.state.visible && composited, boxrig_x11_scrollbar_pixel_width());
        (void)boxrig_x11_terminal_component_quiesce(component);
        return status;
    }
    component->composited_scrollbar_dragging = false;
    if (component_renderer_set_composited_scrollbar_interaction(component, false, false) == -1) return BOXRIG_UI_UNAVAILABLE;
    if (component->scrollbar != NULL)
        boxrig_x11_scrollbar_set_visible(component->scrollbar, show);
    boxrig_ui_scrollbar_visibility_commit(&component->scrollbar_visibility);
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(boxrig_ui_model_terminal(component->model), &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

boxrig_ui_status boxrig_x11_terminal_component_set_scrollbar_visibility(
    boxrig_x11_terminal_component *component, boxrig_ui_scrollbar_visibility policy)
{
    if (component == NULL || policy < BOXRIG_UI_SCROLLBAR_ALWAYS ||
        policy > BOXRIG_UI_SCROLLBAR_NEVER ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) return BOXRIG_UI_INVALID_ARGUMENT;
    if (component->dispatched_grid_resize.revision != 0 || component->zoom_generation_active ||
        component->allocation_transition_pending) return BOXRIG_UI_BUSY;
    boxrig_ui_status status = boxrig_ui_model_synchronize(component->model);
    if (status != BOXRIG_UI_OK) return status;
    boxrig_ui_allocation_input input = {0};
    if (component->allocated) {
        boxrig_font_effective_metrics metrics;
        if (boxrig_x11_terminal_component_get_effective_metrics(component, &metrics) != BOXRIG_FONT_CATALOG_OK)
            return BOXRIG_UI_UNAVAILABLE;
        input.allocation_width = (boxrig_ui_fixed)component->allocation.width * BOXRIG_UI_FIXED_ONE;
        input.allocation_height = (boxrig_ui_fixed)component->allocation.height * BOXRIG_UI_FIXED_ONE;
        input.cell_width = (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
        input.cell_height = (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
        input.cell_metrics_revision = metrics.layout_revision;
        input.window_state = component->allocation.window_state;
    }
    status = boxrig_ui_scrollbar_visibility_prepare(&component->scrollbar_visibility, component->view,
        policy, component->scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE,
        boxrig_x11_scrollbar_pixel_width(), component->allocated ? &input : NULL);
    if (status != BOXRIG_UI_OK || !component->scrollbar_visibility.state.pending) return status;
    boxrig_ui_layout_action action;
    if (!boxrig_ui_view_peek_layout_action(component->view, &action))
        return commit_scrollbar_visibility(component);
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(boxrig_ui_model_terminal(component->model), &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

boxrig_ui_status boxrig_x11_terminal_component_get_scrollbar_visibility(
    const boxrig_x11_terminal_component *component, boxrig_ui_scrollbar_visibility_state *state_out)
{
    if (component == NULL || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = component->scrollbar_visibility.state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_get_layout(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_layout_result *layout_out)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_layout(component->view, layout_out);
}

boxrig_ui_status boxrig_x11_terminal_component_set_preferred_grid(
    boxrig_x11_terminal_component *component,
    size_t rows,
    size_t columns)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_set_preferred_grid(
        component->view,
        rows,
        columns);
}

boxrig_ui_status boxrig_x11_terminal_component_get_preferred_grid(
    const boxrig_x11_terminal_component *component,
    size_t *rows_out,
    size_t *columns_out)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_preferred_grid(
        component->view,
        rows_out,
        columns_out);
}

boxrig_ui_status boxrig_x11_terminal_component_get_size_command_state(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_size_command_state *state_out)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_get_size_command_state(
                     component->view,
                     state_out);
}

bool boxrig_x11_terminal_component_take_layout_action(
    boxrig_x11_terminal_component *component,
    boxrig_ui_layout_action *action_out)
{
    boxrig_ui_size_command_state state;
    boxrig_ui_layout_action action;
    boxrig_ui_layout_result layout;
    bool candidate_required;

    if (component == NULL || action_out == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle) ||
        boxrig_ui_view_get_size_command_state(component->view, &state) !=
            BOXRIG_UI_OK) {
        return false;
    }
    if (state == BOXRIG_UI_SIZE_COMMAND_READY) {
        if (!boxrig_ui_view_peek_layout_action(component->view, &action) ||
            action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            return false;
        }
        candidate_required =
            component->zoom_generation_active &&
            action.value.content_size.cause ==
                BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
        if ((candidate_required &&
             (!component->surface_candidate_ready ||
              boxrig_ui_view_get_layout(component->view, &layout) !=
                  BOXRIG_UI_OK ||
              component->surface_candidate_width != layout.grid_width ||
              component->surface_candidate_height != layout.grid_height)) ||
            !boxrig_ui_view_take_layout_action(component->view, &action) ||
            action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            return false;
        }
        if (candidate_required) {
            component->surface_candidate_revision = action.revision;
        }
        *action_out = action;
        return true;
    }
    if (state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED ||
        component->dispatched_grid_resize.revision != 0 ||
        !component->allocated ||
        !boxrig_ui_view_take_layout_action(component->view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        return false;
    }
    component->dispatched_grid_resize.revision = action.revision;
    component->dispatched_grid_resize.rows = action.value.grid_resize.rows;
    component->dispatched_grid_resize.columns =
        action.value.grid_resize.columns;
    *action_out = action;
    return true;
}

boxrig_ui_status boxrig_x11_terminal_component_complete_size_command(
    boxrig_x11_terminal_component *component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    boxrig_ui_status status;
    bool candidate_accepted;

    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    candidate_accepted = component->surface_candidate_ready &&
                         revision == component->surface_candidate_revision &&
                         outcome == BOXRIG_UI_SIZE_ACCEPTED &&
                         allocation_width ==
                             component->surface_candidate_width &&
                         allocation_height ==
                             component->surface_candidate_height;
    status = boxrig_ui_view_complete_size_command(
        component->view,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
    if (status == BOXRIG_UI_OK && applied_out != NULL && *applied_out) {
        /*
         * Completing negotiation does not itself resize the native child.
         * Keep the committed pixels until allocate applies this exact result.
         */
        component->transition_allocation_width = allocation_width;
        component->transition_allocation_height = allocation_height;
        component->allocation_transition_pending = true;
        component->surface_candidate_accepted = candidate_accepted;
    }
    return status;
}

boxrig_ui_status boxrig_x11_terminal_component_complete_grid_resize(
    boxrig_x11_terminal_component *component,
    uint64_t revision,
    bool succeeded)
{
    boxrig_terminal *terminal;
    size_t rows;
    size_t columns;

    if (component == NULL || revision == 0 ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->dispatched_grid_resize.revision != revision) {
        return BOXRIG_UI_STALE;
    }
    if (!succeeded) {
        return boxrig_x11_terminal_component_quiesce(component);
    }
    terminal = boxrig_ui_model_terminal(component->model);
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows != component->dispatched_grid_resize.rows ||
        columns != component->dispatched_grid_resize.columns) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(
        &component->dispatched_grid_resize,
        0,
        sizeof(component->dispatched_grid_resize));
    return commit_scrollbar_visibility(component);
}

static boxrig_ui_status reconfigure_layout_for_font(
    boxrig_x11_terminal_component *component,
    const boxrig_font_effective_metrics *metrics,
    boxrig_ui_layout_change_kind kind)
{
    boxrig_ui_status font_binding = boxrig_x11_renderer_bind_view(component->renderer, component->view);
    if (font_binding != BOXRIG_UI_OK) return font_binding;
    boxrig_ui_layout_change_input input;
    boxrig_ui_layout_result layout;

    if (!component->allocated) {
        return BOXRIG_UI_OK;
    }
    memset(&input, 0, sizeof(input));
    input.kind = kind;
    input.cell_width =
        (boxrig_ui_fixed)metrics->cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics->cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics->layout_revision;
    return boxrig_ui_view_apply_layout_change(
        component->view,
        &input,
        &layout);
}

static boxrig_ui_status set_component_font(
    boxrig_x11_terminal_component *component,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_ui_layout_change_kind change_kind,
    bool update_raw_state)
{
    boxrig_x11_renderer_font_candidate *candidate = NULL;
    boxrig_x11_font_specification_storage storage;
    boxrig_font_effective_metrics metrics;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (component == NULL || specification == NULL || policy == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    memset(&storage, 0, sizeof(storage));
    font_status = boxrig_x11_renderer_prepare_font(
        component->renderer,
        specification,
        policy,
        &candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return status_from_font_catalog(font_status);
    }
    font_status = boxrig_x11_renderer_font_candidate_metrics(
        candidate,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_x11_renderer_font_candidate_destroy(candidate);
        return status_from_font_catalog(font_status);
    }
    status = boxrig_ui_view_validate_cell_metrics(component->view, &metrics);
    if (status != BOXRIG_UI_OK) {
        boxrig_x11_renderer_font_candidate_destroy(candidate);
        return status;
    }
    if (update_raw_state &&
        !font_specification_storage_copy(specification, &storage)) {
        boxrig_x11_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (component->surface_candidate_ready) {
        component_renderer_abort_surface_candidate(component);
        clear_surface_candidate_state(component);
    }
    if (component_renderer_prepare_font(
            component,
            specification,
            policy,
            &metrics,
            component->direction_policy) == -1) {
        if (update_raw_state) {
            font_specification_storage_destroy(&storage);
        }
        boxrig_x11_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_ui_view_is_layout_authority(component->view)) {
        status = boxrig_ui_view_commit_cell_metrics(component->view, &metrics);
        if (status != BOXRIG_UI_OK) {
            component_renderer_abort_font(component);
            if (update_raw_state) {
                font_specification_storage_destroy(&storage);
            }
            boxrig_x11_renderer_font_candidate_destroy(candidate);
            return status;
        }
    }
    font_status = boxrig_x11_renderer_commit_font(
        component->renderer,
        candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        component_renderer_abort_font(component);
        if (update_raw_state) {
            font_specification_storage_destroy(&storage);
        }
        boxrig_x11_renderer_font_candidate_destroy(candidate);
        return status_from_font_catalog(font_status);
    }
    if (component_renderer_commit_font(component) == -1) {
        component_renderer_abort_font(component);
        if (update_raw_state) {
            font_specification_storage_destroy(&storage);
        }
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_x11_input_adapter_set_cell_size(
            component->input,
            metrics.cell_width,
            metrics.cell_height) == -1) {
        if (update_raw_state) {
            font_specification_storage_destroy(&storage);
        }
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = reconfigure_layout_for_font(
        component,
        &metrics,
        change_kind);
    if (status != BOXRIG_UI_OK) {
        if (update_raw_state) {
            font_specification_storage_destroy(&storage);
        }
        return status;
    }
    if (update_raw_state) {
        font_specification_storage_destroy(&component->font);
        component->font = storage;
        component->rendering_policy = *policy;
        boxrig_font_visual_step_sequence_destroy(component->visual_steps);
        component->visual_steps = NULL;
        component->zoom_generation_active = false;
        component->zoom_target_pending = false;
        component->visual_step_policy_valid = false;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_set_font_specification(
    boxrig_x11_terminal_component *component,
    const boxrig_font_specification *specification)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return set_component_font(
        component,
        specification,
        &component->rendering_policy,
        BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS,
        true);
}

boxrig_ui_status boxrig_x11_terminal_component_get_font_specification(
    const boxrig_x11_terminal_component *component,
    boxrig_font_specification *specification_out)
{
    if (component == NULL || specification_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *specification_out = component->font.specification;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_set_font_rendering_policy(
    boxrig_x11_terminal_component *component,
    const boxrig_font_rendering_policy *policy)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return set_component_font(
        component,
        &component->font.specification,
        policy,
        BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS,
        true);
}

boxrig_ui_status boxrig_x11_terminal_component_get_font_rendering_policy(
    const boxrig_x11_terminal_component *component,
    boxrig_font_rendering_policy *policy_out)
{
    if (component == NULL || policy_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *policy_out = component->rendering_policy;
    return BOXRIG_UI_OK;
}

boxrig_font_catalog_status
boxrig_x11_terminal_component_get_effective_metrics(
    const boxrig_x11_terminal_component *component,
    boxrig_font_effective_metrics *metrics_out)
{
    if (component == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_x11_renderer_get_effective_metrics(
        component->renderer,
        metrics_out);
}

boxrig_font_catalog_status
boxrig_x11_terminal_component_register_font_file(
    boxrig_x11_terminal_component *component,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_x11_renderer_register_font_file(
        component->renderer,
        path,
        path_length,
        face_index,
        resource_id_out);
}

boxrig_font_catalog_status
boxrig_x11_terminal_component_register_font_memory(
    boxrig_x11_terminal_component *component,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_x11_renderer_register_font_memory(
        component->renderer,
        data,
        data_length,
        face_index,
        resource_id_out);
}

boxrig_font_resource_id
boxrig_x11_terminal_component_primary_font_resource(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL
               ? BOXRIG_FONT_RESOURCE_NONE
               : boxrig_x11_renderer_primary_font_resource(
                     component->renderer);
}

uint64_t boxrig_x11_terminal_component_font_catalog_revision(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL
               ? 0
               : boxrig_x11_renderer_font_catalog_revision(
                     component->renderer);
}

boxrig_ui_status
boxrig_x11_terminal_component_set_line_height_adjustment(
    boxrig_x11_terminal_component *component,
    float adjustment_dip)
{
    boxrig_font_specification specification;

    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    specification = component->font.specification;
    specification.line_height_adjustment_dip = adjustment_dip;
    return set_component_font(
        component,
        &specification,
        &component->rendering_policy,
        BOXRIG_UI_LAYOUT_CHANGE_LINE_HEIGHT,
        true);
}

boxrig_ui_status
boxrig_x11_terminal_component_get_line_height_adjustment(
    const boxrig_x11_terminal_component *component,
    float *adjustment_dip_out)
{
    if (component == NULL || adjustment_dip_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *adjustment_dip_out =
        component->font.specification.line_height_adjustment_dip;
    return BOXRIG_UI_OK;
}

static bool visual_step_policy_matches(
    const boxrig_x11_terminal_component *component,
    boxrig_font_visual_step_policy policy)
{
    return component->visual_step_policy_valid &&
           component->visual_step_policy.minimum_size_dip ==
               policy.minimum_size_dip &&
           component->visual_step_policy.maximum_size_dip ==
               policy.maximum_size_dip &&
           component->visual_step_policy.candidate_increment_dip ==
               policy.candidate_increment_dip;
}

static boxrig_font_catalog_status resolve_component_zoom_metrics(
    float requested_size_dip,
    void *context,
    boxrig_font_effective_metrics *metrics_out)
{
    boxrig_x11_terminal_component *component = context;
    boxrig_x11_renderer_font_candidate *candidate = NULL;
    boxrig_font_specification specification;
    boxrig_font_catalog_status status;

    if (component == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    specification = component->font.specification;
    specification.size_dip = requested_size_dip;
    specification.line_height_adjustment_dip = 0.0f;
    status = boxrig_x11_renderer_prepare_font(
        component->renderer,
        &specification,
        &component->rendering_policy,
        &candidate);
    if (status == BOXRIG_FONT_CATALOG_OK) {
        status = boxrig_x11_renderer_font_candidate_metrics(
            candidate,
            metrics_out);
    }
    boxrig_x11_renderer_font_candidate_destroy(candidate);
    return status;
}

static boxrig_ui_status commit_component_zoom(
    boxrig_x11_terminal_component *component,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_font_specification specification = component->font.specification;

    specification.size_dip = metrics->requested_size_dip;
    return set_component_font(
        component,
        &specification,
        &component->rendering_policy,
        BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS,
        false);
}

static boxrig_ui_status queue_or_commit_component_zoom(
    boxrig_x11_terminal_component *component,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_ui_status status;

    /* Keep the generation already sent to the host stable. Later key repeats
     * replace one desired target until that generation has been presented. */
    if (component->zoom_generation_active) {
        if (metrics->requested_size_dip ==
            component->active_zoom_metrics.requested_size_dip) {
            component->zoom_target_pending = false;
        } else {
            component->pending_zoom_metrics = *metrics;
            component->zoom_target_pending = true;
        }
        return BOXRIG_UI_OK;
    }
    status = commit_component_zoom(component, metrics);
    if (status == BOXRIG_UI_OK) {
        component->active_zoom_metrics = *metrics;
        component->zoom_generation_active = true;
    }
    return status;
}

static boxrig_ui_status complete_component_zoom_generation(
    boxrig_x11_terminal_component *component)
{
    boxrig_font_effective_metrics target;
    boxrig_ui_status status;

    if (!component->zoom_generation_active) {
        return BOXRIG_UI_OK;
    }
    /* The completed frame is the user-visible checkpoint. Only now may the
     * latest target become a new metrics and content-size generation. */
    component->zoom_generation_active = false;
    if (!component->zoom_target_pending) {
        return BOXRIG_UI_OK;
    }
    target = component->pending_zoom_metrics;
    component->zoom_target_pending = false;
    status = commit_component_zoom(component, &target);
    if (status != BOXRIG_UI_OK) {
        component->pending_zoom_metrics = target;
        component->zoom_target_pending = true;
        return status;
    }
    component->active_zoom_metrics = target;
    component->zoom_generation_active = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_visual_zoom(
    boxrig_x11_terminal_component *component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    boxrig_font_visual_step_sequence *sequence = NULL;
    boxrig_font_effective_metrics current;
    boxrig_font_effective_metrics candidate;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (component == NULL || changed_out == NULL ||
        (direction != BOXRIG_FONT_ZOOM_DECREASE &&
         direction != BOXRIG_FONT_ZOOM_INCREASE) ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    if (component->visual_steps == NULL ||
        !visual_step_policy_matches(component, policy)) {
        font_status = boxrig_x11_renderer_get_effective_metrics(
            component->renderer,
            &current);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return status_from_font_catalog(font_status);
        }
        font_status = boxrig_font_visual_step_sequence_create(
            &current,
            component->font.specification.line_height_adjustment_dip,
            policy,
            resolve_component_zoom_metrics,
            component,
            &sequence);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return status_from_font_catalog(font_status);
        }
        boxrig_font_visual_step_sequence_destroy(component->visual_steps);
        component->visual_steps = sequence;
        component->visual_step_policy = policy;
        component->visual_step_policy_valid = true;
    }
    if (!boxrig_font_visual_step_sequence_peek(
            component->visual_steps,
            direction,
            &candidate)) {
        return BOXRIG_UI_OK;
    }
    status = queue_or_commit_component_zoom(component, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_advance(
        component->visual_steps,
        direction);
    *changed_out = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_visual_zoom_reset(
    boxrig_x11_terminal_component *component,
    bool *changed_out)
{
    boxrig_font_effective_metrics candidate;
    boxrig_ui_status status;

    if (component == NULL || changed_out == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    if (!boxrig_font_visual_step_sequence_peek_reset(
            component->visual_steps,
            &candidate)) {
        return BOXRIG_UI_OK;
    }
    status = queue_or_commit_component_zoom(component, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_reset(component->visual_steps);
    *changed_out = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_set_text_direction(
    boxrig_x11_terminal_component *component,
    boxrig_text_direction_policy policy)
{
    boxrig_terminal *terminal;
    boxrig_ui_dirty dirty;
    size_t rows;
    size_t columns;

    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle) ||
        policy < BOXRIG_TEXT_DIRECTION_AUTO ||
        policy > BOXRIG_TEXT_DIRECTION_FORCE_RTL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->direction_policy == policy) {
        return BOXRIG_UI_OK;
    }
    if (component_renderer_set_text_direction(component, policy) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_x11_renderer_set_direction_policy(
            component->renderer,
            policy) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    component->direction_policy = policy;
    terminal = boxrig_ui_model_terminal(component->model);
    boxrig_terminal_size(terminal, &rows, &columns);
    dirty.row = 0;
    dirty.row_count = rows;
    dirty.column = 0;
    dirty.column_count = columns;
    if (boxrig_ui_view_invalidate(
            component->view,
            BOXRIG_UI_DIRTY_PRESENT,
            dirty) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

boxrig_text_direction_policy
boxrig_x11_terminal_component_text_direction(
    const boxrig_x11_terminal_component *component)
{
    return component == NULL ? BOXRIG_TEXT_DIRECTION_AUTO
                             : component->direction_policy;
}

boxrig_ui_status
boxrig_x11_terminal_component_set_frame_request_callback(
    boxrig_x11_terminal_component *component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_set_frame_request_callback(
        component->view,
        callback,
        context);
}

boxrig_ui_status boxrig_x11_terminal_component_request_focus(
    boxrig_x11_terminal_component *component)
{
    if (component == NULL || !component->visible ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return component->native_operations->request_focus(component);
}

bool boxrig_x11_terminal_component_focused(
    const boxrig_x11_terminal_component *component)
{
    return component != NULL && component->focused;
}

boxrig_ui_status boxrig_x11_terminal_component_set_input_enabled(
    boxrig_x11_terminal_component *component, bool enabled)
{
    if (component == NULL || !boxrig_ui_lifecycle_accepts_events(&component->lifecycle))
        return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_input_state before, after;
    (void)boxrig_x11_input_adapter_get_input_state(component->input, &before);
    boxrig_ui_status status = boxrig_x11_input_adapter_set_input_enabled(component->input, enabled);
    (void)boxrig_x11_input_adapter_get_input_state(component->input, &after);
    if (before.generation != after.generation || before.phase != after.phase) {
        size_t rows, columns;
        boxrig_terminal_size(boxrig_ui_model_terminal(component->model), &rows, &columns);
        boxrig_ui_dirty dirty = {0, rows, 0, columns};
        if (boxrig_ui_view_set_composition_active(component->view, false) != BOXRIG_UI_OK ||
            boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_OVERLAY, dirty) != BOXRIG_UI_OK)
            return BOXRIG_UI_UNAVAILABLE;
    }
    return status;
}

boxrig_ui_status boxrig_x11_terminal_component_get_input_state(
    const boxrig_x11_terminal_component *component, boxrig_ui_input_state *state_out)
{
    if (component == NULL || !boxrig_ui_lifecycle_accepts_events(&component->lifecycle))
        return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_x11_input_adapter_get_input_state(component->input, state_out);
}

boxrig_ui_status boxrig_x11_terminal_component_set_input_config(
    boxrig_x11_terminal_component *component, const boxrig_input_config *config)
{
    if (component == NULL || !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_x11_input_adapter_set_input_config(component->input, config);
}

boxrig_ui_status boxrig_x11_terminal_component_set_key_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_key_router router,
    void *context)
{
    if (component == NULL ||
        boxrig_x11_input_adapter_set_key_router(
            component->input,
            router,
            context) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_set_mouse_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_mouse_router router,
    void *context)
{
    if (component == NULL ||
        boxrig_x11_input_adapter_set_mouse_router(
            component->input,
            router == NULL ? NULL : component->view,
            router,
            context) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_view_action_capabilities
boxrig_x11_terminal_component_view_action_capabilities(
    const boxrig_x11_terminal_component *component)
{
    return boxrig_x11_input_adapter_view_action_capabilities(
        component == NULL ? NULL : component->input);
}

boxrig_ui_status boxrig_x11_terminal_component_apply_view_action(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_view_action *action)
{
    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_x11_input_adapter_apply_view_action(
        component->input,
        component->view,
        action);
}

boxrig_ui_status boxrig_x11_terminal_component_queue_paste(
    boxrig_x11_terminal_component *component,
    const uint8_t *bytes,
    size_t length)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_input_state state;
    (void)boxrig_x11_input_adapter_get_input_state(component->input, &state);
    if (state.phase != BOXRIG_UI_INPUT_ENABLED) return BOXRIG_UI_UNAVAILABLE;
    if (boxrig_x11_input_adapter_queue_paste(
            component->input,
            bytes,
            length) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return length == 0 ? BOXRIG_UI_OK :
        boxrig_ui_view_notify_local_input(component->view);
}

boxrig_ui_status boxrig_x11_terminal_component_hit_test(
    boxrig_x11_terminal_component *component,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_hit_test_with_composition(
        component->view,
        boxrig_x11_input_adapter_composition(component->input),
        x,
        y,
        result_out);
}

boxrig_ui_status boxrig_x11_terminal_component_get_scrollbar_state(
    boxrig_x11_terminal_component *component, boxrig_scrollbar_state *state_out)
{
    return component == NULL ? BOXRIG_UI_INVALID_ARGUMENT
        : boxrig_ui_view_get_scrollbar_state(component->view, state_out);
}

boxrig_ui_status boxrig_x11_terminal_component_get_scrollbar_model(
    boxrig_x11_terminal_component *component,
    boxrig_scrollbar_model *model_out)
{
    boxrig_terminal *terminal;

    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    terminal = boxrig_ui_model_terminal(component->model);
    return boxrig_scrollbar_model_read(terminal, model_out) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_status boxrig_x11_terminal_component_apply_scrollbar_action(
    boxrig_x11_terminal_component *component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_ui_view_apply_scrollbar_action(component->view, action, position, changed_out);
}

boxrig_ui_status boxrig_x11_terminal_component_set_transparency(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_state previous;
    boxrig_ui_transparency_state candidate;
    boxrig_ui_status status;

    if (component == NULL || config == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_get_transparency(component->view, &previous);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_ui_transparency_resolve(
        config,
        &previous.capabilities,
        &previous,
        &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (component_renderer_apply_transparency_state(
            component,
            &candidate) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_ui_view_set_transparency_config(component->view, config);
    if (status != BOXRIG_UI_OK) {
        (void)component_renderer_apply_transparency_state(
            component,
            &previous);
        return status;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_get_transparency(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_transparency_state *state_out)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_transparency(component->view, state_out);
}

uint32_t boxrig_x11_terminal_component_capabilities(
    const boxrig_x11_terminal_component *component)
{
    boxrig_ui_transparency_state transparency;
    uint32_t capabilities;

    if (component == NULL) {
        return 0;
    }
    capabilities = BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY |
                   BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES |
                   BOXRIG_UI_COMPONENT_CAPABILITY_INPUT_METHOD |
                   BOXRIG_UI_COMPONENT_CAPABILITY_CLIPBOARD_PASTE |
                   BOXRIG_UI_COMPONENT_CAPABILITY_SIZE_COMMAND;
    if (component->native_hosting == BOXRIG_UI_COMPONENT_OWNED_CHILD) {
        capabilities |=
            BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD;
    } else {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_BORROWED_TARGET |
                        BOXRIG_UI_COMPONENT_CAPABILITY_EXTERNAL_PRESENTATION;
    }
    if (component->scrollbar != NULL) {
        capabilities |=
            BOXRIG_UI_COMPONENT_CAPABILITY_NATIVE_CHILD_SCROLLBAR;
    }
    if (component->scrollbar_presentation ==
        BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED) {
        capabilities |=
            BOXRIG_UI_COMPONENT_CAPABILITY_COMPOSITED_SCROLLBAR;
    }
    if (boxrig_ui_view_get_transparency(
            component->view,
            &transparency) == BOXRIG_UI_OK &&
        transparency.capabilities.premultiplied_argb32_target) {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA;
    }
    return capabilities;
}

boxrig_ui_status boxrig_x11_terminal_component_get_diagnostic(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_state transparency;

    if (component == NULL || diagnostic_out == NULL ||
        boxrig_x11_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_ui_view_get_transparency(
            component->view,
            &transparency) != BOXRIG_UI_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    diagnostic_out->running =
        boxrig_ui_lifecycle_accepts_events(&component->lifecycle);
    if (diagnostic_out->running &&
        boxrig_ui_view_animation_status(component->view) != BOXRIG_UI_OK) {
        diagnostic_out->running = false;
        diagnostic_out->failed = true;
        diagnostic_out->failure_reason =
            BOXRIG_UI_COMPONENT_FAILURE_ANIMATION;
    }
    diagnostic_out->visible = component->visible;
    diagnostic_out->focused = component->focused;
    diagnostic_out->input_blocked =
        boxrig_x11_input_adapter_blocked(component->input);
    diagnostic_out->layout_authority =
        boxrig_ui_view_is_layout_authority(component->view);
    diagnostic_out->layout_configured = component->allocated;
    diagnostic_out->metrics_revision = metrics.layout_revision;
    diagnostic_out->transparency_revision = transparency.revision;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_x11_terminal_component_set_visible(
    boxrig_x11_terminal_component *component,
    bool visible)
{
    boxrig_ui_status status;

    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_set_visible(component->view, visible);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    component->visible = visible;
    component->native_operations->set_visible(component, visible);
    return BOXRIG_UI_OK;
}

bool boxrig_x11_terminal_component_visible(
    const boxrig_x11_terminal_component *component)
{
    return component != NULL && component->visible;
}

static boxrig_ui_status synchronize_animation_composition(
    boxrig_x11_terminal_component *component)
{
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(
            boxrig_x11_input_adapter_composition(component->input), &composition) !=
        BOXRIG_COMPOSITION_OK) return BOXRIG_UI_UNAVAILABLE;
    /* Preedit visibility is laid out by the renderer later in this frame.
     * Stop its cursor clock as soon as the focused composition is active. */
    return boxrig_ui_view_set_composition_active(component->view,
        composition.active && composition.focused);
}

boxrig_ui_component_event_result
boxrig_x11_terminal_component_dispatch_event(
    boxrig_x11_terminal_component *component,
    const XEvent *event)
{
    int result;

    if (component == NULL || event == NULL) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (!boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    if (component->scrollbar_visibility.state.pending &&
        (event->type == ButtonPress || event->type == ButtonRelease || event->type == MotionNotify))
        return BOXRIG_UI_COMPONENT_EVENT_BLOCKED;
    if (component->scrollbar != NULL) {
        result = boxrig_x11_scrollbar_process_event(
            component->scrollbar,
            event);
        if (result == -1) {
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        }
        if (result == 1) {
            return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
        }
    }
    result = process_composited_scrollbar_event(component, event);
    if (result == -1) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (result == 1) {
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    }
    if (!event_targets_window(event, component->window)) {
        result = boxrig_x11_input_adapter_process_event(
            component->input,
            event);
        if (result == -1) {
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        }
        if (result == 1 && event->type == SelectionNotify &&
            boxrig_ui_view_notify_local_input(component->view) != BOXRIG_UI_OK)
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        return result == 1 ? BOXRIG_UI_COMPONENT_EVENT_HANDLED
                           : BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    boxrig_ui_input_state input_state;
    (void)boxrig_x11_input_adapter_get_input_state(component->input, &input_state);
    if (input_state.phase == BOXRIG_UI_INPUT_ENABLED &&
        boxrig_x11_input_adapter_blocked(component->input) &&
        event->type != SelectionNotify &&
        event->type != SelectionRequest &&
        event->type != SelectionClear &&
        event->type != PropertyNotify &&
        event->type != DestroyNotify) {
        return BOXRIG_UI_COMPONENT_EVENT_BLOCKED;
    }
    if (event->type == Expose &&
        component_renderer_expose(
            component,
            event->xexpose.x,
            event->xexpose.y,
            event->xexpose.width > 0
                ? (unsigned int)event->xexpose.width
                : 1,
            event->xexpose.height > 0
                ? (unsigned int)event->xexpose.height
                : 1) == -1) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    result = boxrig_x11_input_adapter_process_event(component->input, event);
    if (result == -1) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (result == 1 && (event->type == FocusIn || event->type == FocusOut)) {
        bool focused = event->type == FocusIn;
        if (boxrig_ui_view_set_focused(component->view, focused) != BOXRIG_UI_OK) {
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        }
        component->focused = focused;
    }
    if (result == 1 &&
        ((event->type == KeyPress || event->type == SelectionNotify) &&
         boxrig_ui_view_notify_local_input(component->view) != BOXRIG_UI_OK)) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (synchronize_animation_composition(component) != BOXRIG_UI_OK) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (result == 1 || event->type == Expose ||
        event->type == ConfigureNotify) {
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    }
    return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
}

boxrig_ui_status boxrig_x11_terminal_component_frame(
    boxrig_x11_terminal_component *component,
    boxrig_ui_component_frame_result *result_out)
{
    boxrig_terminal *terminal;
    bool grid_resize_ready;
    bool waiting_for_grid_resize;
    bool waiting_for_allocation;

    if (component == NULL || result_out == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_status animation_status = boxrig_ui_view_animation_status(component->view);
    if (animation_status != BOXRIG_UI_OK) return animation_status;
    animation_status = synchronize_animation_composition(component);
    if (animation_status == BOXRIG_UI_OK) {
        animation_status = boxrig_ui_model_synchronize(component->model);
    }
    if (animation_status != BOXRIG_UI_OK) return animation_status;
    terminal = boxrig_ui_model_terminal(component->model);
    if (terminal == NULL ||
        boxrig_x11_input_adapter_retry(component->input) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_status scrollbar_status = boxrig_x11_terminal_component_set_scrollbar_visibility(
        component, component->scrollbar_visibility.state.requested);
    if (scrollbar_status != BOXRIG_UI_OK && scrollbar_status != BOXRIG_UI_BUSY) return scrollbar_status;
    waiting_for_grid_resize = grid_resize_waiting_for_application(
        component, &grid_resize_ready);
    if (!waiting_for_grid_resize &&
        prepare_zoom_surface_candidate(component) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    waiting_for_allocation = presentation_waits_for_allocation(component);
    if (!waiting_for_grid_resize && !waiting_for_allocation) {
        if (component_renderer_draw_view_composition(
                component,
                component->view,
                boxrig_x11_input_adapter_composition(component->input)) ==
                -1 ||
            boxrig_x11_input_adapter_synchronize_candidate(component->input) ==
                -1 ||
            (component->scrollbar != NULL &&
             boxrig_x11_scrollbar_synchronize(component->scrollbar) == -1)) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        if (!boxrig_ui_view_frame_requested(component->view) &&
            complete_component_zoom_generation(component) != BOXRIG_UI_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        waiting_for_allocation = presentation_waits_for_allocation(component);
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->frame_pending =
        waiting_for_grid_resize ||
        (!waiting_for_allocation &&
         boxrig_ui_view_frame_requested(component->view));
    result_out->input_blocked =
        boxrig_x11_input_adapter_blocked(component->input);
    result_out->additional_dispatch =
        grid_resize_ready || result_out->input_blocked ||
        (!waiting_for_grid_resize &&
         (result_out->frame_pending ||
          surface_candidate_preparation_pending(component) ||
          size_command_ready_for_dispatch(component)));
    result_out->has_animation_deadline =
        boxrig_terminal_next_animation_deadline(
            terminal,
            &result_out->animation_deadline_milliseconds);
    uint64_t blink_deadline = 0;
    if (boxrig_ui_view_next_animation_deadline(component->view, &blink_deadline) &&
        (!result_out->has_animation_deadline ||
         blink_deadline < result_out->animation_deadline_milliseconds)) {
        result_out->has_animation_deadline = true;
        result_out->animation_deadline_milliseconds = blink_deadline;
    }
    return BOXRIG_UI_OK;
}
