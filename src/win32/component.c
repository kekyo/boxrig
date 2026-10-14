/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/component.h"

#include "boxrig/ui/lifecycle.h"
#include "../ui/scrollbar_visibility.h"
#include "boxrig/win32/renderer.h"
#include "renderer_private.h"
#include "component_private.h"
#include "animation.h"
#include "input_private.h"
#include "../ui/view_private.h"

#include <windowsx.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BOXRIG_WIN32_DEFAULT_PASTE_CAPACITY (8U * 1024U * 1024U)
#define BOXRIG_WIN32_DEFAULT_PASTE_CHUNK_SIZE 4096U

typedef struct boxrig_win32_font_specification_storage {
    boxrig_font_specification specification;
    boxrig_font_family *families;
    boxrig_font_feature *features;
    boxrig_font_variation *variations;
} boxrig_win32_font_specification_storage;

typedef struct boxrig_win32_component_native_operations {
    bool (*set_geometry)(
        boxrig_win32_terminal_component *component,
        const boxrig_ui_component_allocation *allocation,
        unsigned int scrollbar_width);
    void (*set_visible)(
        boxrig_win32_terminal_component *component,
        bool visible);
    boxrig_ui_status (*request_focus)(
        boxrig_win32_terminal_component *component);
    void (*destroy)(boxrig_win32_terminal_component *component);
} boxrig_win32_component_native_operations;

struct boxrig_win32_terminal_component {
    HINSTANCE instance;
    HWND parent;
    HWND window;
    HWND input_window;
    HWND scrollbar;
    boxrig_ui_component_native_hosting native_hosting;
    boxrig_ui_component_scrollbar_presentation scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_controller scrollbar_visibility;
    bool scrollbar_committing;
    const boxrig_win32_component_native_operations *native_operations;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_win32_animation *animations;
    boxrig_win32_renderer *renderer;
    const boxrig_win32_component_presentation *presentation;
    void *presentation_context;
    boxrig_win32_component_renderer_factory renderer_factory;
    void *renderer_factory_context;
    boxrig_win32_input_adapter *input;
    boxrig_ui_lifecycle lifecycle;
    boxrig_win32_font_specification_storage font;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_visual_step_sequence *visual_steps;
    boxrig_font_visual_step_policy visual_step_policy;
    boxrig_font_effective_metrics active_zoom_metrics;
    boxrig_font_effective_metrics pending_zoom_metrics;
    boxrig_text_direction_policy direction_policy;
    boxrig_win32_component_present_callback presenter;
    void *presenter_context;
    boxrig_ui_component_allocation allocation;
    unsigned int target_width;
    unsigned int target_height;
    boxrig_ui_fixed transition_allocation_width;
    boxrig_ui_fixed transition_allocation_height;
    boxrig_ui_fixed surface_candidate_width;
    boxrig_ui_fixed surface_candidate_height;
    uint64_t surface_candidate_revision;
    boxrig_ui_grid_resize dispatched_grid_resize;
    boxrig_win32_ime_placement_status ime_placement;
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
    bool composited_scrollbar_button_down;
    bool composited_scrollbar_captured;
    int composited_scrollbar_drag_anchor;
    int composited_scrollbar_wheel_delta;
};

static int component_set_surface_size(boxrig_win32_terminal_component *component, unsigned int width, unsigned int height)
{
    if (component->presentation != NULL) {
        return component->presentation->resize(component->presentation_context, width, height);
    }
    return boxrig_win32_renderer_set_surface_size(component->renderer, width, height);
}

static int component_prepare_surface_candidate(boxrig_win32_terminal_component *component, unsigned int width, unsigned int height)
{
    if (component->presentation != NULL) {
        return component->presentation->prepare(component->presentation_context, width, height);
    }
    return boxrig_win32_renderer_prepare_surface_candidate(component->renderer, width, height);
}

static int component_commit_surface_candidate(boxrig_win32_terminal_component *component)
{
    if (component->presentation != NULL) {
        return component->presentation->commit(component->presentation_context);
    }
    return boxrig_win32_renderer_commit_surface_candidate(component->renderer);
}

static void component_abort_surface_candidate(boxrig_win32_terminal_component *component)
{
    if (component->presentation != NULL) {
        component->presentation->abort(component->presentation_context);
        return;
    }
    boxrig_win32_renderer_abort_surface_candidate(component->renderer);
}

static int component_update_view_composition(boxrig_win32_terminal_component *component, boxrig_ui_view *view, boxrig_composition *composition)
{
    if (component->presentation != NULL) {
        return component->presentation->draw(component->presentation_context, view, composition);
    }
    return boxrig_win32_renderer_update_view_composition(component->renderer, view, composition);
}

static int component_set_composited_scrollbar(boxrig_win32_terminal_component *component, bool enabled, unsigned int width)
{
    if (component->presentation != NULL) {
        return component->presentation->scrollbar(component->presentation_context, enabled, width);
    }
    return boxrig_win32_renderer_set_composited_scrollbar(component->renderer, enabled, width);
}

static int component_set_composited_scrollbar_interaction(boxrig_win32_terminal_component *component, bool hovered, bool dragging)
{
    if (component->presentation != NULL) {
        return component->presentation->interaction(component->presentation_context, hovered, dragging);
    }
    return boxrig_win32_renderer_set_composited_scrollbar_interaction(component->renderer, hovered, dragging);
}

static int cancel_scrollbar_drag(boxrig_win32_terminal_component *component)
{
    bool active = component->composited_scrollbar_dragging || component->composited_scrollbar_captured;
    bool captured = component->composited_scrollbar_captured;
    component->composited_scrollbar_dragging = false;
    component->composited_scrollbar_captured = false;
    /* ReleaseCapture synchronously reports capture loss. Clear our ownership
     * first, and preserve the pending button release when hiding mid-gesture. */
    if (captured && GetCapture() == component->input_window && !ReleaseCapture()) return -1;
    return active ? component_set_composited_scrollbar_interaction(component, false, false) : 0;
}

static int component_get_surface(boxrig_win32_terminal_component *component, HDC *surface, SIZE *size)
{
    if (component->presentation != NULL) {
        *surface = NULL;
        return component->presentation->size(component->presentation_context, size);
    }
    return boxrig_win32_renderer_get_surface(component->renderer, surface, size);
}

static bool owned_child_set_geometry(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    unsigned int scrollbar_width)
{
    if (component->scrollbar != NULL &&
        !SetWindowPos(
            component->scrollbar,
            NULL,
            (int)(allocation->width - scrollbar_width),
            0,
            (int)scrollbar_width,
            (int)allocation->height,
            SWP_NOZORDER | SWP_NOACTIVATE)) {
        return false;
    }
    return SetWindowPos(
               component->window,
               NULL,
               allocation->x,
               allocation->y,
               (int)allocation->width,
               (int)allocation->height,
               SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

static void owned_child_set_visible(
    boxrig_win32_terminal_component *component,
    bool visible)
{
    if (component->window != NULL) {
        ShowWindow(component->window, visible ? SW_SHOWNA : SW_HIDE);
    }
    if (component->scrollbar != NULL) {
        ShowWindow(component->scrollbar, visible && component->scrollbar_visibility.state.visible ? SW_SHOWNA : SW_HIDE);
    }
}

static boxrig_ui_status owned_child_request_focus(
    boxrig_win32_terminal_component *component)
{
    (void)SetFocus(component->input_window);
    return GetFocus() == component->input_window ? BOXRIG_UI_OK
                                                 : BOXRIG_UI_UNAVAILABLE;
}

static void owned_child_destroy(boxrig_win32_terminal_component *component)
{
    if (component->window != NULL) {
        (void)SetWindowLongPtrW(component->window, GWLP_USERDATA, 0);
        (void)DestroyWindow(component->window);
        component->window = NULL;
        component->input_window = NULL;
        component->scrollbar = NULL;
    }
}

static const boxrig_win32_component_native_operations
    owned_child_native_operations = {
        owned_child_set_geometry,
        owned_child_set_visible,
        owned_child_request_focus,
        owned_child_destroy};

static bool borrowed_target_set_geometry(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    unsigned int scrollbar_width)
{
    (void)component;
    (void)allocation;
    (void)scrollbar_width;
    return true;
}

static void borrowed_target_set_visible(
    boxrig_win32_terminal_component *component,
    bool visible)
{
    (void)component;
    (void)visible;
}

static boxrig_ui_status borrowed_target_request_focus(
    boxrig_win32_terminal_component *component)
{
    (void)SetFocus(component->input_window);
    return GetFocus() == component->input_window ? BOXRIG_UI_OK
                                                 : BOXRIG_UI_UNAVAILABLE;
}

static void borrowed_target_destroy(
    boxrig_win32_terminal_component *component)
{
    component->window = NULL;
    component->input_window = NULL;
    component->presenter = NULL;
    component->presenter_context = NULL;
}

static const boxrig_win32_component_native_operations
    borrowed_target_native_operations = {
        borrowed_target_set_geometry,
        borrowed_target_set_visible,
        borrowed_target_request_focus,
        borrowed_target_destroy};

static LRESULT CALLBACK component_window_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter);

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

static void font_specification_storage_destroy(
    boxrig_win32_font_specification_storage *storage)
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
    boxrig_win32_font_specification_storage *storage_out)
{
    boxrig_win32_font_specification_storage storage;
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

static unsigned int platform_scrollbar_pixel_width(void)
{
    int width;

    width = GetSystemMetrics(SM_CXVSCROLL);
    return width > 0 ? (unsigned int)width : 1;
}

static unsigned int scrollbar_pixel_width(const boxrig_win32_terminal_component *component)
{
    return component->scrollbar_committing
        ? component->scrollbar_visibility.target_width
        : component->scrollbar_visibility.state.reserved_width;
}

static unsigned int component_content_width(
    const boxrig_win32_terminal_component *component,
    unsigned int width)
{
    unsigned int scrollbar_width = scrollbar_pixel_width(component);

    return width > scrollbar_width ? width - scrollbar_width : 1;
}

static void clear_surface_candidate_state(
    boxrig_win32_terminal_component *component)
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
    const boxrig_win32_terminal_component *component)
{
    boxrig_ui_size_command_state state;

    return component->allocation_transition_pending ||
           (boxrig_ui_view_get_size_command_state(
                component->view,
                &state) == BOXRIG_UI_OK &&
            state != BOXRIG_UI_SIZE_COMMAND_IDLE);
}

static bool surface_candidate_preparation_pending(
    const boxrig_win32_terminal_component *component)
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
    const boxrig_win32_terminal_component *component)
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
    const boxrig_win32_terminal_component *component,
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
    boxrig_win32_terminal_component *component, bool *deferred_out)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(component->model);
    boxrig_ui_layout_action action;
    boxrig_ui_layout_result layout;
    unsigned int content_width;
    unsigned int height;
    unsigned int scrollbar_width =
        component->scrollbar_presentation ==
                BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED
            ? scrollbar_pixel_width(component)
            : 0;
    int prepare_status;

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
        content_width > UINT_MAX - scrollbar_width) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    prepare_status = component_prepare_surface_candidate(component,
        content_width + scrollbar_width, height);
    if (prepare_status < 0) return BOXRIG_UI_UNAVAILABLE;
    if (prepare_status > 0) { *deferred_out = true; return BOXRIG_UI_OK; }
    prepare_status = component_update_view_composition(component,
            component->view,
            boxrig_win32_input_adapter_composition(component->input));
    if (prepare_status < 0) {
        component_abort_surface_candidate(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (prepare_status > 0) { *deferred_out = true; return BOXRIG_UI_OK; }
    component->surface_candidate_width = layout.grid_width;
    component->surface_candidate_height = layout.grid_height;
    component->surface_candidate_revision = 0;
    component->surface_candidate_ready = true;
    component->surface_candidate_accepted = false;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status present_entire_surface(
    boxrig_win32_terminal_component *component)
{
    HDC surface;
    SIZE size;
    RECT dirty;

    if (component->presenter == NULL) {
        return BOXRIG_UI_OK;
    }
    if (component_get_surface(component,
            &surface,
            &size) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    dirty.left = 0;
    dirty.top = 0;
    dirty.right = size.cx;
    dirty.bottom = size.cy;
    return component->presenter(
        component->presenter_context,
        surface,
        size,
        &dirty);
}

static boxrig_ui_status configure_transparency(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state state;
    boxrig_ui_status status;

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target =
        (component->presentation == NULL &&
         boxrig_win32_renderer_supports_alpha(component->renderer));
    if (capabilities.premultiplied_argb32_target) {
        capabilities.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    if (component->presentation != NULL && component->presentation->transparency != NULL)
        component->presentation->transparency(component->presentation_context, &capabilities);
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
    if (status != BOXRIG_UI_OK || state.effective == BOXRIG_UI_TRANSPARENCY_UNAVAILABLE ||
        (component->presentation == NULL &&
         boxrig_win32_renderer_apply_transparency_state(
            component->renderer,
            &state) == -1)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

static bool register_component_class(HINSTANCE instance)
{
    static const WCHAR class_name[] = L"boxrig_terminal_component";
    WNDCLASSEXW window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    /* Native multi-click promotion is required by selection and link gestures. */
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = component_window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    window_class.lpszClassName = class_name;
    if (window_class.hCursor == NULL) {
        return false;
    }
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static boxrig_ui_status initialize_component_resources(
    boxrig_win32_terminal_component *component,
    const boxrig_win32_terminal_component_config *config)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(component->model);
    boxrig_font_effective_metrics metrics;
    boxrig_font_specification default_specification;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;
    HDC reference_context;

    if (terminal == NULL || component->input_window == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    reference_context = CreateCompatibleDC(NULL);
    if (reference_context == NULL ||
        boxrig_win32_renderer_create(
            reference_context,
            &component->renderer) == -1) {
        if (reference_context != NULL) {
            (void)DeleteDC(reference_context);
        }
        return BOXRIG_UI_UNAVAILABLE;
    }
    (void)DeleteDC(reference_context);
    if (component->renderer_factory != NULL) {
        status = component->renderer_factory(component->window,
            component->input_window, component->renderer,
            component->renderer_factory_context,
            &component->presentation_context, &component->presentation);
        if (status != BOXRIG_UI_OK) return status;
    }
    font_status = boxrig_win32_renderer_get_effective_metrics(
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
    status = boxrig_win32_animation_acquire(&component->animations);
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_animation_runtime_attach(
            boxrig_win32_animation_runtime(component->animations), component->view);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_require_animation_runtime(component->view);
    }
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
    if (component_set_composited_scrollbar(component,
            config->scrollbar_presentation ==
                BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED,
            platform_scrollbar_pixel_width()) == -1 ||
        boxrig_win32_input_adapter_create(
            component->input_window,
            terminal,
            metrics.cell_width,
            metrics.cell_height,
            config->paste_capacity,
            config->paste_chunk_size,
            &component->input) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_win32_input_adapter_set_animation_view(component->input, component->view);
    if (config->scrollbar_presentation ==
        BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD) {
        component->scrollbar = CreateWindowExW(
            0,
            L"SCROLLBAR",
            L"",
            WS_CHILD | SBS_VERT,
            0,
            0,
            1,
            1,
            component->window,
            NULL,
            component->instance,
            NULL);
        if (component->scrollbar == NULL) {
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    if (boxrig_ui_view_set_visible(component->view, false) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    default_specification = boxrig_font_specification_default();
    if (!font_specification_storage_copy(
            &default_specification,
            &component->font)) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->rendering_policy = boxrig_font_rendering_policy_default();
    return BOXRIG_UI_OK;
}

static bool borrowed_targets_compatible(
    const boxrig_win32_terminal_borrowed_target *target,
    RECT *presentation_client_out)
{
    DWORD presentation_process = 0;
    DWORD input_process = 0;
    DWORD presentation_thread = GetWindowThreadProcessId(
        target->presentation_window,
        &presentation_process);
    DWORD input_thread = GetWindowThreadProcessId(
        target->input_window,
        &input_process);
    RECT presentation_client;

    if (presentation_thread == 0 || input_thread == 0 ||
        presentation_thread != GetCurrentThreadId() ||
        input_thread != presentation_thread ||
        presentation_process != GetCurrentProcessId() ||
        input_process != presentation_process ||
        !GetClientRect(
            target->presentation_window,
            &presentation_client) ||
        presentation_client.right <= presentation_client.left ||
        presentation_client.bottom <= presentation_client.top) {
        return false;
    }
    *presentation_client_out = presentation_client;
    return true;
}

boxrig_win32_terminal_component_config
boxrig_win32_terminal_component_config_default(void)
{
    boxrig_win32_terminal_component_config config;

    config.view = boxrig_ui_view_config_default();
    config.attachment = BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    config.scrollbar_presentation =
        BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD;
    config.paste_capacity = BOXRIG_WIN32_DEFAULT_PASTE_CAPACITY;
    config.paste_chunk_size = BOXRIG_WIN32_DEFAULT_PASTE_CHUNK_SIZE;
    config.transparency = boxrig_ui_transparency_config_default();
    return config;
}

boxrig_ui_status boxrig_win32_terminal_component_create_child(
    HINSTANCE instance,
    HWND parent,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out)
{
    return boxrig_win32_terminal_component_create_child_with_renderer(instance, parent, model, config,
        0, NULL, NULL, component_out);
}

boxrig_ui_status boxrig_win32_terminal_component_create_child_with_renderer(
    HINSTANCE instance,
    HWND parent,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    DWORD extended_style,
    boxrig_win32_component_renderer_factory renderer_factory,
    void *renderer_factory_context,
    boxrig_win32_terminal_component **component_out)
{
    static const WCHAR class_name[] = L"boxrig_terminal_component";
    boxrig_win32_terminal_component *component;
    boxrig_ui_status status;

    if (instance == NULL || parent == NULL || !IsWindow(parent) ||
        model == NULL || config == NULL || component_out == NULL ||
        !valid_attachment(config->attachment) ||
        !valid_scrollbar_presentation(config->scrollbar_presentation) ||
        config->paste_chunk_size == 0) {
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
    component->renderer_factory = renderer_factory;
    component->renderer_factory_context = renderer_factory_context;
    component->instance = instance;
    component->parent = parent;
    component->model = model;
    component->native_hosting = BOXRIG_UI_COMPONENT_OWNED_CHILD;
    component->scrollbar_presentation = config->scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_initialize(&component->scrollbar_visibility,
        config->scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE, platform_scrollbar_pixel_width());
    component->native_operations = &owned_child_native_operations;
    component->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    component->ime_placement = BOXRIG_WIN32_IME_PLACEMENT_OK;
    boxrig_ui_lifecycle_initialize(&component->lifecycle);
    if (!register_component_class(instance)) {
        boxrig_win32_terminal_component_destroy(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    component->window = CreateWindowExW(
        extended_style,
        class_name,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        1,
        1,
        parent,
        NULL,
        instance,
        component);
    component->input_window = component->window;
    if (component->window == NULL) {
        boxrig_win32_terminal_component_destroy(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = initialize_component_resources(component, config);
    if (status != BOXRIG_UI_OK) {
        boxrig_win32_terminal_component_destroy(component);
        return status;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_terminal_component_create_on_target(
    HINSTANCE instance,
    const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out)
{
    return boxrig_win32_terminal_component_create_on_target_with_renderer(instance, target, model, config,
        NULL, NULL, component_out);
}

boxrig_ui_status boxrig_win32_terminal_component_create_on_target_with_renderer(
    HINSTANCE instance,
    const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_component_renderer_factory renderer_factory,
    void *renderer_factory_context,
    boxrig_win32_terminal_component **component_out)
{
    boxrig_win32_terminal_component *component;
    RECT presentation_client;
    boxrig_ui_status status;

    if (instance == NULL || target == NULL ||
        target->presentation_window == NULL ||
        !IsWindow(target->presentation_window) ||
        target->input_window == NULL || !IsWindow(target->input_window) ||
        (target->presenter == NULL && renderer_factory == NULL) ||
        model == NULL || config == NULL ||
        component_out == NULL || !valid_attachment(config->attachment) ||
        !valid_scrollbar_presentation(config->scrollbar_presentation) ||
        config->scrollbar_presentation ==
            BOXRIG_UI_COMPONENT_SCROLLBAR_NATIVE_CHILD ||
        config->paste_chunk_size == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (boxrig_ui_model_terminal(model) == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!borrowed_targets_compatible(target, &presentation_client)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->renderer_factory = renderer_factory;
    component->renderer_factory_context = renderer_factory_context;
    component->instance = instance;
    component->window = target->presentation_window;
    component->input_window = target->input_window;
    component->model = model;
    component->native_hosting = BOXRIG_UI_COMPONENT_BORROWED_TARGET;
    component->scrollbar_presentation = config->scrollbar_presentation;
    boxrig_ui_scrollbar_visibility_initialize(&component->scrollbar_visibility,
        config->scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE, platform_scrollbar_pixel_width());
    component->native_operations = &borrowed_target_native_operations;
    component->presenter = renderer_factory == NULL ? target->presenter : NULL;
    component->presenter_context = target->presenter_context;
    component->target_width =
        (unsigned int)(presentation_client.right - presentation_client.left);
    component->target_height =
        (unsigned int)(presentation_client.bottom - presentation_client.top);
    component->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    component->ime_placement = BOXRIG_WIN32_IME_PLACEMENT_OK;
    boxrig_ui_lifecycle_initialize(&component->lifecycle);
    status = initialize_component_resources(component, config);
    if (status != BOXRIG_UI_OK) {
        boxrig_win32_terminal_component_destroy(component);
        return status;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_terminal_component_quiesce(
    boxrig_win32_terminal_component *component)
{
    boxrig_ui_lifecycle_state state;

    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_scrollbar_visibility_cancel(&component->scrollbar_visibility);
    if (cancel_scrollbar_drag(component) == -1) return BOXRIG_UI_UNAVAILABLE;
    boxrig_ui_animation_runtime_detach(component->view);
    boxrig_win32_animation_release(component->animations);
    component->animations = NULL;
    boxrig_win32_input_adapter_set_animation_view(component->input, NULL);
    state = boxrig_ui_lifecycle_get_state(&component->lifecycle);
    if (state == BOXRIG_UI_LIFECYCLE_RUNNING &&
        boxrig_ui_lifecycle_begin_shutdown(&component->lifecycle) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->view != NULL &&
        (boxrig_ui_view_set_visible(component->view, false) != BOXRIG_UI_OK ||
         boxrig_ui_view_set_focused(component->view, false) != BOXRIG_UI_OK)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->presentation != NULL) {
        component->presentation->visible(component->presentation_context, false);
    }
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

void boxrig_win32_terminal_component_destroy(
    boxrig_win32_terminal_component *component)
{
    if (component == NULL) {
        return;
    }
    (void)boxrig_win32_terminal_component_quiesce(component);
    if (component->presentation != NULL) {
        component->presentation->destroy(component->presentation_context);
        component->presentation = NULL;
        component->presentation_context = NULL;
    }
    if (component->native_operations != NULL) {
        component->native_operations->destroy(component);
    }
    (void)boxrig_ui_lifecycle_mark_detached(&component->lifecycle);
    boxrig_win32_input_adapter_destroy(component->input);
    boxrig_win32_renderer_destroy(component->renderer);
    boxrig_ui_view_destroy(component->view);
    boxrig_font_visual_step_sequence_destroy(component->visual_steps);
    font_specification_storage_destroy(&component->font);
    (void)boxrig_ui_lifecycle_mark_stopped(&component->lifecycle);
    free(component);
}

boxrig_ui_component_native_hosting
boxrig_win32_terminal_component_native_hosting(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ? BOXRIG_UI_COMPONENT_OWNED_CHILD
                             : component->native_hosting;
}

HWND boxrig_win32_terminal_component_owned_window(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ||
                   component->native_hosting != BOXRIG_UI_COMPONENT_OWNED_CHILD
               ? NULL
               : component->window;
}

HWND boxrig_win32_terminal_component_presentation_window(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ? NULL : component->window;
}

HWND boxrig_win32_terminal_component_input_window(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ? NULL : component->input_window;
}

bool boxrig_win32_terminal_component_input_window_owned(
    const boxrig_win32_terminal_component *component)
{
    return component != NULL && component->input_window != NULL &&
           component->native_hosting == BOXRIG_UI_COMPONENT_OWNED_CHILD;
}

HWND boxrig_win32_terminal_component_scrollbar_window(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ? NULL : component->scrollbar;
}

boxrig_ui_status boxrig_win32_terminal_component_set_target_extent(
    boxrig_win32_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    if (component == NULL || width == 0 || height == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->native_hosting != BOXRIG_UI_COMPONENT_BORROWED_TARGET) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    component->target_width = width;
    component->target_height = height;
    return BOXRIG_UI_OK;
}

boxrig_ui_view *boxrig_win32_terminal_component_view(
    boxrig_win32_terminal_component *component)
{
    return component == NULL ? NULL : component->view;
}

boxrig_composition *boxrig_win32_terminal_component_composition(
    boxrig_win32_terminal_component *component)
{
    return component == NULL
               ? NULL
               : boxrig_win32_input_adapter_composition(component->input);
}

boxrig_ui_model *boxrig_win32_terminal_component_model(
    boxrig_win32_terminal_component *component)
{
    return component == NULL ? NULL : component->model;
}

boxrig_ui_status
boxrig_win32_terminal_component_set_editable_document_provider(
    boxrig_win32_terminal_component *component,
    bool available)
{
    boxrig_ui_editable_document *document;

    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    document = available
                   ? boxrig_ui_view_editable_document(component->view)
                   : NULL;
    return boxrig_win32_input_adapter_set_editable_document(
               component->input,
               document) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_BUSY;
}

boxrig_ui_status boxrig_win32_terminal_component_allocate(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_status status;
    unsigned int content_width;
    unsigned int surface_width;
    unsigned int scrollbar_width;
    HDC previous_surface;
    SIZE previous_surface_size;
    int viewport_x;
    int viewport_y;
    bool preserve_candidate;
    bool commit_candidate;
    bool resolve_candidate;
    bool completion_echo;
    bool surface_resized = false;

    if (component == NULL || allocation == NULL || layout_out == NULL ||
        allocation->width == 0 || allocation->height == 0 ||
        component->window == NULL ||
        component->input_window == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle) ||
        boxrig_win32_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->native_hosting == BOXRIG_UI_COMPONENT_BORROWED_TARGET &&
        (allocation->x < 0 || allocation->y < 0 ||
         (unsigned int)allocation->x > component->target_width ||
         allocation->width >
             component->target_width - (unsigned int)allocation->x ||
         (unsigned int)allocation->y > component->target_height ||
         allocation->height >
             component->target_height - (unsigned int)allocation->y)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->scrollbar_visibility.state.pending && !component->scrollbar_committing)
        return BOXRIG_UI_BUSY;
    content_width = component_content_width(component, allocation->width);
    surface_width = component->scrollbar_presentation ==
                            BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED
                        ? allocation->width
                        : content_width;
    viewport_x = component->native_hosting ==
                         BOXRIG_UI_COMPONENT_BORROWED_TARGET
                     ? allocation->x
                     : 0;
    viewport_y = component->native_hosting ==
                         BOXRIG_UI_COMPONENT_BORROWED_TARGET
                     ? allocation->y
                     : 0;
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
    if (commit_candidate && component->presentation == NULL &&
        component_commit_surface_candidate(component) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (commit_candidate &&
        present_entire_surface(component) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (resolve_candidate && !commit_candidate) {
        component_abort_surface_candidate(component);
    }
    if (resolve_candidate && !(commit_candidate && component->presentation != NULL)) {
        clear_surface_candidate_state(component);
    }
    if (!preserve_candidate && !commit_candidate) {
        surface_resized =
            component_get_surface(component,
                &previous_surface,
                &previous_surface_size) == -1 ||
            previous_surface_size.cx != (LONG)surface_width ||
            previous_surface_size.cy != (LONG)allocation->height;
        if (component_set_surface_size(component,
                surface_width,
                allocation->height) == -1) {
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    if (boxrig_win32_input_adapter_set_cell_size(
            component->input,
            metrics.cell_width,
            metrics.cell_height) == -1 ||
        boxrig_win32_input_adapter_set_viewport_origin(
            component->input,
            viewport_x,
            viewport_y) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    scrollbar_width = scrollbar_pixel_width(component);
    if (!component->native_operations->set_geometry(
            component,
            allocation,
            scrollbar_width)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->presentation != NULL &&
        (component->presentation->geometry(component->presentation_context,
             viewport_x, viewport_y, surface_width, allocation->height) == -1 ||
         (commit_candidate && component_commit_surface_candidate(component) == -1))) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    /* A managed exchange can fail after geometry has been applied. Keep the
     * accepted size transaction available for the caller's allocation retry. */
    if (commit_candidate && component->presentation != NULL) {
        clear_surface_candidate_state(component);
    }
    if (surface_resized && component->allocated && component->presentation == NULL &&
        ((component->presenter != NULL &&
          present_entire_surface(component) != BOXRIG_UI_OK) ||
         (component->presenter == NULL &&
          (!InvalidateRect(component->window, NULL, FALSE) ||
           !UpdateWindow(component->window))))) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (commit_candidate && component->presenter == NULL &&
        (!InvalidateRect(component->window, NULL, FALSE) ||
         !UpdateWindow(component->window))) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (commit_candidate && component->presentation == NULL &&
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

static boxrig_ui_status commit_scrollbar_visibility(boxrig_win32_terminal_component *component)
{
    if (!component->scrollbar_visibility.state.pending) return BOXRIG_UI_OK;
    bool show = component->scrollbar_visibility.target_visible;
    bool composited = component->scrollbar_presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    if (component_set_composited_scrollbar(component, show && composited, platform_scrollbar_pixel_width()) == -1)
        return BOXRIG_UI_UNAVAILABLE;
    component->scrollbar_committing = true;
    boxrig_ui_status status = BOXRIG_UI_OK;
    if (component->allocated) {
        boxrig_ui_layout_result layout;
        status = boxrig_win32_terminal_component_allocate(component, &component->allocation, &layout);
    }
    component->scrollbar_committing = false;
    if (status != BOXRIG_UI_OK) {
        (void)component_set_composited_scrollbar(component,
            component->scrollbar_visibility.state.visible && composited, platform_scrollbar_pixel_width());
        (void)boxrig_win32_terminal_component_quiesce(component);
        return status;
    }
    if (cancel_scrollbar_drag(component) == -1) return BOXRIG_UI_UNAVAILABLE;
    if (component_set_composited_scrollbar_interaction(component, false, false) == -1) return BOXRIG_UI_UNAVAILABLE;
    component->composited_scrollbar_wheel_delta = 0;
    if (component->scrollbar != NULL) {
        if (!show && GetCapture() == component->scrollbar) ReleaseCapture();
        ShowWindow(component->scrollbar, show && component->visible ? SW_SHOWNA : SW_HIDE);
    }
    boxrig_ui_scrollbar_visibility_commit(&component->scrollbar_visibility);
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(boxrig_ui_model_terminal(component->model), &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

boxrig_ui_status boxrig_win32_terminal_component_set_scrollbar_visibility(
    boxrig_win32_terminal_component *component, boxrig_ui_scrollbar_visibility policy)
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
        if (boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) != BOXRIG_FONT_CATALOG_OK)
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
        platform_scrollbar_pixel_width(), component->allocated ? &input : NULL);
    if (status != BOXRIG_UI_OK || !component->scrollbar_visibility.state.pending) return status;
    boxrig_ui_layout_action action;
    if (!boxrig_ui_view_peek_layout_action(component->view, &action))
        return commit_scrollbar_visibility(component);
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(boxrig_ui_model_terminal(component->model), &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

boxrig_ui_status boxrig_win32_terminal_component_get_scrollbar_visibility(
    const boxrig_win32_terminal_component *component, boxrig_ui_scrollbar_visibility_state *state_out)
{
    if (component == NULL || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = component->scrollbar_visibility.state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_terminal_component_get_layout(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_layout_result *layout_out)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_get_layout(component->view, layout_out);
}

boxrig_ui_status boxrig_win32_terminal_component_set_preferred_grid(
    boxrig_win32_terminal_component *component,
    size_t rows,
    size_t columns)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_set_preferred_grid(
                     component->view,
                     rows,
                     columns);
}

boxrig_ui_status boxrig_win32_terminal_component_get_preferred_grid(
    const boxrig_win32_terminal_component *component,
    size_t *rows_out,
    size_t *columns_out)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_get_preferred_grid(
                     component->view,
                     rows_out,
                     columns_out);
}

boxrig_ui_status boxrig_win32_terminal_component_get_size_command_state(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_size_command_state *state_out)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_get_size_command_state(
                     component->view,
                     state_out);
}

bool boxrig_win32_terminal_component_take_layout_action(
    boxrig_win32_terminal_component *component,
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

boxrig_ui_status boxrig_win32_terminal_component_complete_size_command(
    boxrig_win32_terminal_component *component,
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
         * A caller may separate negotiation completion from native resize.
         * Keep the committed surface until allocate applies this result.
         */
        component->transition_allocation_width = allocation_width;
        component->transition_allocation_height = allocation_height;
        component->allocation_transition_pending = true;
        component->surface_candidate_accepted = candidate_accepted;
    }
    return status;
}

boxrig_ui_status boxrig_win32_terminal_component_complete_grid_resize(
    boxrig_win32_terminal_component *component,
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
        return boxrig_win32_terminal_component_quiesce(component);
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
    boxrig_win32_terminal_component *component,
    const boxrig_font_effective_metrics *metrics,
    boxrig_ui_layout_change_kind kind)
{
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
    boxrig_win32_terminal_component *component,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_ui_layout_change_kind change_kind,
    bool update_raw_state)
{
    boxrig_win32_renderer_font_candidate *candidate = NULL;
    boxrig_win32_font_specification_storage storage;
    boxrig_font_effective_metrics metrics;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (component == NULL || specification == NULL || policy == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    memset(&storage, 0, sizeof(storage));
    font_status = boxrig_win32_renderer_prepare_font(
        component->renderer,
        specification,
        policy,
        &candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return status_from_font_catalog(font_status);
    }
    font_status = boxrig_win32_renderer_font_candidate_metrics(
        candidate,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_win32_renderer_font_candidate_destroy(candidate);
        return status_from_font_catalog(font_status);
    }
    status = boxrig_ui_view_validate_cell_metrics(component->view, &metrics);
    if (status != BOXRIG_UI_OK) {
        boxrig_win32_renderer_font_candidate_destroy(candidate);
        return status;
    }
    if (update_raw_state &&
        !font_specification_storage_copy(specification, &storage)) {
        boxrig_win32_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (update_raw_state && component->surface_candidate_ready) {
        component_abort_surface_candidate(component);
        clear_surface_candidate_state(component);
    }
    if (boxrig_ui_view_is_layout_authority(component->view)) {
        status = boxrig_ui_view_commit_cell_metrics(component->view, &metrics);
        if (status != BOXRIG_UI_OK) {
            if (update_raw_state) {
                font_specification_storage_destroy(&storage);
            }
            boxrig_win32_renderer_font_candidate_destroy(candidate);
            return status;
        }
    }
    font_status = boxrig_win32_renderer_commit_font(
        component->renderer,
        candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        if (update_raw_state) {
            font_specification_storage_destroy(&storage);
        }
        boxrig_win32_renderer_font_candidate_destroy(candidate);
        return status_from_font_catalog(font_status);
    }
    if (boxrig_win32_input_adapter_set_cell_size(
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

boxrig_ui_status boxrig_win32_terminal_component_set_font_specification(
    boxrig_win32_terminal_component *component,
    const boxrig_font_specification *specification)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : set_component_font(
                     component,
                     specification,
                     &component->rendering_policy,
                     BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS,
                     true);
}

boxrig_ui_status boxrig_win32_terminal_component_get_font_specification(
    const boxrig_win32_terminal_component *component,
    boxrig_font_specification *specification_out)
{
    if (component == NULL || specification_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *specification_out = component->font.specification;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_terminal_component_set_font_rendering_policy(
    boxrig_win32_terminal_component *component,
    const boxrig_font_rendering_policy *policy)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : set_component_font(
                     component,
                     &component->font.specification,
                     policy,
                     BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS,
                     true);
}

boxrig_ui_status boxrig_win32_terminal_component_get_font_rendering_policy(
    const boxrig_win32_terminal_component *component,
    boxrig_font_rendering_policy *policy_out)
{
    if (component == NULL || policy_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *policy_out = component->rendering_policy;
    return BOXRIG_UI_OK;
}

boxrig_font_catalog_status
boxrig_win32_terminal_component_get_effective_metrics(
    const boxrig_win32_terminal_component *component,
    boxrig_font_effective_metrics *metrics_out)
{
    return component == NULL
               ? BOXRIG_FONT_CATALOG_INVALID_ARGUMENT
               : boxrig_win32_renderer_get_effective_metrics(
                     component->renderer,
                     metrics_out);
}

boxrig_font_catalog_status
boxrig_win32_terminal_component_register_font_file(
    boxrig_win32_terminal_component *component,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_win32_renderer_register_font_file(
        component->renderer,
        path,
        path_length,
        face_index,
        resource_id_out);
}

boxrig_font_catalog_status
boxrig_win32_terminal_component_register_font_memory(
    boxrig_win32_terminal_component *component,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_win32_renderer_register_font_memory(
        component->renderer,
        data,
        data_length,
        face_index,
        resource_id_out);
}

boxrig_font_resource_id
boxrig_win32_terminal_component_primary_font_resource(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL
               ? BOXRIG_FONT_RESOURCE_NONE
               : boxrig_win32_renderer_primary_font_resource(
                     component->renderer);
}

uint64_t boxrig_win32_terminal_component_font_catalog_revision(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL
               ? 0
               : boxrig_win32_renderer_font_catalog_revision(
                     component->renderer);
}

boxrig_ui_status
boxrig_win32_terminal_component_set_line_height_adjustment(
    boxrig_win32_terminal_component *component,
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
boxrig_win32_terminal_component_get_line_height_adjustment(
    const boxrig_win32_terminal_component *component,
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
    const boxrig_win32_terminal_component *component,
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
    boxrig_win32_terminal_component *component = context;
    boxrig_win32_renderer_font_candidate *candidate = NULL;
    boxrig_font_specification specification;
    boxrig_font_catalog_status status;

    if (component == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    specification = component->font.specification;
    specification.size_dip = requested_size_dip;
    specification.line_height_adjustment_dip = 0.0f;
    status = boxrig_win32_renderer_prepare_font(
        component->renderer,
        &specification,
        &component->rendering_policy,
        &candidate);
    if (status == BOXRIG_FONT_CATALOG_OK) {
        status = boxrig_win32_renderer_font_candidate_metrics(
            candidate,
            metrics_out);
    }
    boxrig_win32_renderer_font_candidate_destroy(candidate);
    return status;
}

static boxrig_ui_status commit_component_zoom(
    boxrig_win32_terminal_component *component,
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
    boxrig_win32_terminal_component *component,
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
    boxrig_win32_terminal_component *component)
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

boxrig_ui_status boxrig_win32_terminal_component_visual_zoom(
    boxrig_win32_terminal_component *component,
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
        font_status = boxrig_win32_renderer_get_effective_metrics(
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

boxrig_ui_status boxrig_win32_terminal_component_visual_zoom_reset(
    boxrig_win32_terminal_component *component,
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

boxrig_ui_status boxrig_win32_terminal_component_set_text_direction(
    boxrig_win32_terminal_component *component,
    boxrig_text_direction_policy policy)
{
    boxrig_terminal *terminal;
    boxrig_ui_dirty dirty;
    size_t rows;
    size_t columns;

    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle) ||
        boxrig_win32_renderer_set_direction_policy(
            component->renderer,
            policy) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->direction_policy == policy) {
        return BOXRIG_UI_OK;
    }
    component->direction_policy = policy;
    if (component->presentation != NULL) {
        component->presentation->direction(component->presentation_context, policy);
    }
    terminal = boxrig_ui_model_terminal(component->model);
    boxrig_terminal_size(terminal, &rows, &columns);
    dirty.row = 0;
    dirty.row_count = rows;
    dirty.column = 0;
    dirty.column_count = columns;
    return boxrig_ui_view_invalidate(
               component->view,
               BOXRIG_UI_DIRTY_PRESENT,
               dirty) == BOXRIG_UI_OK
               ? BOXRIG_UI_OK
               : BOXRIG_UI_UNAVAILABLE;
}

boxrig_text_direction_policy
boxrig_win32_terminal_component_text_direction(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ? BOXRIG_TEXT_DIRECTION_AUTO
                             : component->direction_policy;
}

boxrig_ui_status
boxrig_win32_terminal_component_set_frame_request_callback(
    boxrig_win32_terminal_component *component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_set_frame_request_callback(
                     component->view,
                     callback,
                     context);
}

boxrig_ui_status boxrig_win32_terminal_component_set_visible(
    boxrig_win32_terminal_component *component,
    bool visible)
{
    boxrig_ui_status status;

    if (component == NULL || component->window == NULL ||
        component->input_window == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_set_visible(component->view, visible);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    component->visible = visible;
    if (!visible && cancel_scrollbar_drag(component) == -1) return BOXRIG_UI_UNAVAILABLE;
    if (component->presentation != NULL) {
        component->presentation->visible(component->presentation_context, visible);
    }
    component->native_operations->set_visible(component, visible);
    return BOXRIG_UI_OK;
}

bool boxrig_win32_terminal_component_visible(
    const boxrig_win32_terminal_component *component)
{
    return component != NULL && component->visible;
}

boxrig_ui_status boxrig_win32_terminal_component_request_focus(
    boxrig_win32_terminal_component *component)
{
    if (component == NULL || component->input_window == NULL ||
        !component->visible ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return component->native_operations->request_focus(component);
}

bool boxrig_win32_terminal_component_focused(
    const boxrig_win32_terminal_component *component)
{
    return component != NULL && component->focused;
}

boxrig_ui_status boxrig_win32_terminal_component_set_input_enabled(
    boxrig_win32_terminal_component *component, bool enabled)
{
    if (component == NULL || !boxrig_ui_lifecycle_accepts_events(&component->lifecycle))
        return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_input_state before, after;
    (void)boxrig_win32_input_adapter_get_input_state(component->input, &before);
    boxrig_ui_status status = boxrig_win32_input_adapter_set_input_enabled(component->input, enabled);
    (void)boxrig_win32_input_adapter_get_input_state(component->input, &after);
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

boxrig_ui_status boxrig_win32_terminal_component_get_input_state(
    const boxrig_win32_terminal_component *component, boxrig_ui_input_state *state_out)
{
    if (component == NULL || !boxrig_ui_lifecycle_accepts_events(&component->lifecycle))
        return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_win32_input_adapter_get_input_state(component->input, state_out);
}

boxrig_ui_status boxrig_win32_terminal_component_set_input_config(
    boxrig_win32_terminal_component *component, const boxrig_input_config *config)
{
    if (component == NULL || !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_win32_input_adapter_set_input_config(component->input, config);
}

boxrig_ui_status boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_key_router router,
    void *context)
{
    return component != NULL &&
                   boxrig_win32_input_adapter_set_key_router(
                       component->input,
                       router,
                       context) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_status boxrig_win32_terminal_component_set_mouse_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_mouse_router router,
    void *context)
{
    return component != NULL &&
                   boxrig_win32_input_adapter_set_mouse_router(
                       component->input,
                       router == NULL ? NULL : component->view,
                       router,
                       context) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_view_action_capabilities
boxrig_win32_terminal_component_view_action_capabilities(
    const boxrig_win32_terminal_component *component)
{
    return boxrig_win32_input_adapter_view_action_capabilities(
        component == NULL ? NULL : component->input);
}

boxrig_ui_status boxrig_win32_terminal_component_apply_view_action(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_view_action *action)
{
    if (component == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_win32_input_adapter_apply_view_action(
        component->input,
        component->view,
        action);
}

boxrig_ui_status boxrig_win32_terminal_component_queue_paste(
    boxrig_win32_terminal_component *component,
    const uint8_t *bytes,
    size_t length)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_input_state state;
    (void)boxrig_win32_input_adapter_get_input_state(component->input, &state);
    if (state.phase != BOXRIG_UI_INPUT_ENABLED) return BOXRIG_UI_UNAVAILABLE;
    return component != NULL &&
                   boxrig_win32_input_adapter_queue_paste(
                       component->input,
                       bytes,
                       length) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_status boxrig_win32_terminal_component_hit_test(
    boxrig_win32_terminal_component *component,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_hit_test_with_composition(
                     component->view,
                     boxrig_win32_input_adapter_composition(component->input),
                     x,
                     y,
                     result_out);
}

boxrig_ui_status boxrig_win32_terminal_component_get_scrollbar_state(
    boxrig_win32_terminal_component *component, boxrig_scrollbar_state *state_out)
{
    return component == NULL ? BOXRIG_UI_INVALID_ARGUMENT
        : boxrig_ui_view_get_scrollbar_state(component->view, state_out);
}

boxrig_ui_status boxrig_win32_terminal_component_get_scrollbar_model(
    boxrig_win32_terminal_component *component,
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

boxrig_ui_status boxrig_win32_terminal_component_apply_scrollbar_action(
    boxrig_win32_terminal_component *component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_ui_view_apply_scrollbar_action(component->view, action, position, changed_out);
}

static int pointer_message_position(
    const boxrig_win32_terminal_component *component,
    UINT message,
    LPARAM long_parameter,
    int64_t *x_out,
    int64_t *y_out)
{
    POINT point;
    int origin_x;
    int origin_y;

    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEMOVE:
        point.x = GET_X_LPARAM(long_parameter);
        point.y = GET_Y_LPARAM(long_parameter);
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        point.x = GET_X_LPARAM(long_parameter);
        point.y = GET_Y_LPARAM(long_parameter);
        if (!ScreenToClient(component->input_window, &point)) {
            return -1;
        }
        break;
    default:
        return 0;
    }
    origin_x = component->native_hosting ==
                       BOXRIG_UI_COMPONENT_BORROWED_TARGET
                   ? component->allocation.x
                   : 0;
    origin_y = component->native_hosting ==
                       BOXRIG_UI_COMPONENT_BORROWED_TARGET
                   ? component->allocation.y
                   : 0;
    *x_out = (int64_t)point.x - origin_x;
    *y_out = (int64_t)point.y - origin_y;
    return 1;
}

static bool pointer_release_message(UINT message)
{
    return message == WM_LBUTTONUP || message == WM_MBUTTONUP ||
           message == WM_RBUTTONUP || message == WM_XBUTTONUP;
}

static bool pointer_press_message(UINT message)
{
    return message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
           message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK ||
           message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK ||
           message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK;
}

static int apply_composited_scrollbar_action(
    boxrig_win32_terminal_component *component,
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

static int process_composited_scrollbar_pointer(
    boxrig_win32_terminal_component *component,
    UINT message,
    WPARAM word_parameter,
    int64_t x,
    int64_t y)
{
    boxrig_terminal *terminal;
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    unsigned int width;
    int64_t left;
    bool inside;

    if (message == WM_LBUTTONUP && component->composited_scrollbar_button_down) {
        component->composited_scrollbar_button_down = false;
        return cancel_scrollbar_drag(component) == -1 ? -1 : 1;
    }
    if (component->scrollbar_presentation !=
            BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED ||
        !component->scrollbar_visibility.state.visible ||
        !component->allocated) {
        return 0;
    }
    width = scrollbar_pixel_width(component);
    if (width > component->allocation.width) {
        width = component->allocation.width;
    }
    left = (int64_t)component->allocation.width - width;
    inside = x >= left && x < component->allocation.width && y >= 0 &&
             y < component->allocation.height;
    if (message == WM_MOUSEMOVE) {
        if (component->composited_scrollbar_dragging) {
            int64_t requested =
                y - component->composited_scrollbar_drag_anchor;
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
                component_set_composited_scrollbar_interaction(component,
                    inside,
                    true) == -1) {
                return -1;
            }
            return 1;
        }
        if (component_set_composited_scrollbar_interaction(component,
                inside,
                false) == -1) {
            return -1;
        }
        return inside ? 1 : 0;
    }
    if (pointer_release_message(message)) {
        bool was_dragging = component->composited_scrollbar_dragging;

        if (message == WM_LBUTTONUP) {
            component->composited_scrollbar_dragging = false;
        }
        if (component_set_composited_scrollbar_interaction(component,
                inside,
                component->composited_scrollbar_dragging) == -1) {
            return -1;
        }
        return inside || was_dragging ? 1 : 0;
    }
    if (!inside) {
        return 0;
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        SetCapture(component->input_window);
        if (GetCapture() != component->input_window) return -1;
        component->composited_scrollbar_captured = true;
        component->composited_scrollbar_button_down = true;
    }
    if (message == WM_MOUSEWHEEL) {
        int64_t accumulated =
            (int64_t)component->composited_scrollbar_wheel_delta +
            GET_WHEEL_DELTA_WPARAM(word_parameter);

        if (accumulated > INT_MAX) {
            accumulated = INT_MAX;
        } else if (accumulated < INT_MIN) {
            accumulated = INT_MIN;
        }
        component->composited_scrollbar_wheel_delta = (int)accumulated;
        while (component->composited_scrollbar_wheel_delta >= WHEEL_DELTA ||
               component->composited_scrollbar_wheel_delta <=
                   -WHEEL_DELTA) {
            boxrig_scrollbar_action action =
                component->composited_scrollbar_wheel_delta > 0
                    ? BOXRIG_SCROLLBAR_LINE_UP
                    : BOXRIG_SCROLLBAR_LINE_DOWN;

            if (apply_composited_scrollbar_action(
                    component,
                    action,
                    0) == -1) {
                return -1;
            }
            component->composited_scrollbar_wheel_delta +=
                component->composited_scrollbar_wheel_delta > 0
                    ? -WHEEL_DELTA
                    : WHEEL_DELTA;
        }
    } else if (message != WM_MOUSEHWHEEL && pointer_press_message(message)) {
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
        if ((message == WM_LBUTTONDOWN ||
             message == WM_LBUTTONDBLCLK) &&
            model.enabled) {
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
        }
    }
    return component_set_composited_scrollbar_interaction(component,
               true,
               component->composited_scrollbar_dragging) == -1
               ? -1
               : 1;
}

static int synchronize_scrollbar(
    boxrig_win32_terminal_component *component)
{
    boxrig_scrollbar_model model;
    SCROLLINFO information;

    if (component->scrollbar == NULL) {
        return 0;
    }
    if (boxrig_win32_terminal_component_get_scrollbar_model(
            component,
            &model) != BOXRIG_UI_OK ||
        model.viewport_rows == 0 || model.viewport_rows > (size_t)INT_MAX ||
        (model.enabled &&
         model.history_rows >
             (size_t)INT_MAX - model.viewport_rows + 1)) {
        return -1;
    }
    memset(&information, 0, sizeof(information));
    information.cbSize = sizeof(information);
    information.fMask =
        SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    information.nMin = 0;
    information.nPage = (UINT)model.viewport_rows;
    if (model.enabled) {
        information.nMax =
            (int)(model.history_rows + model.viewport_rows - 1);
        information.nPos = (int)model.position;
    } else {
        information.nMax = (int)model.viewport_rows - 1;
        information.nPos = 0;
    }
    /* SetScrollInfo returns the resulting position, so zero is a valid
     * result and cannot be interpreted as a failure indication. */
    (void)SetScrollInfo(component->scrollbar, SB_CTL, &information, TRUE);
    return 0;
}

static boxrig_ui_component_event_result dispatch_vertical_scroll(
    boxrig_win32_terminal_component *component,
    WPARAM word_parameter,
    LRESULT *result_out)
{
    boxrig_scrollbar_action action;
    SCROLLINFO information;
    size_t position = 0;
    bool changed;

    switch (LOWORD(word_parameter)) {
    case SB_LINEUP:
        action = BOXRIG_SCROLLBAR_LINE_UP;
        break;
    case SB_LINEDOWN:
        action = BOXRIG_SCROLLBAR_LINE_DOWN;
        break;
    case SB_PAGEUP:
        action = BOXRIG_SCROLLBAR_PAGE_UP;
        break;
    case SB_PAGEDOWN:
        action = BOXRIG_SCROLLBAR_PAGE_DOWN;
        break;
    case SB_TOP:
        action = BOXRIG_SCROLLBAR_TOP;
        break;
    case SB_BOTTOM:
        action = BOXRIG_SCROLLBAR_BOTTOM;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        memset(&information, 0, sizeof(information));
        information.cbSize = sizeof(information);
        information.fMask = SIF_TRACKPOS;
        if (!GetScrollInfo(
                component->scrollbar,
                SB_CTL,
                &information)) {
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        }
        if (information.nTrackPos > 0) {
            position = (size_t)information.nTrackPos;
        }
        action = BOXRIG_SCROLLBAR_POSITION;
        break;
    case SB_ENDSCROLL:
        *result_out = 0;
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    default:
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    if (boxrig_win32_terminal_component_apply_scrollbar_action(
            component,
            action,
            position,
            &changed) != BOXRIG_UI_OK) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    *result_out = 0;
    return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
}

static int present_paint(
    boxrig_win32_terminal_component *component,
    HDC device_context,
    RECT rectangle)
{
    HDC surface_context;
    SIZE surface_size;

    if (component->presentation != NULL) {
        return component->presentation->paint(component->presentation_context);
    }

    if (!component->allocated ||
        component->native_hosting != BOXRIG_UI_COMPONENT_OWNED_CHILD) {
        return 0;
    }
    if (component_get_surface(component,
            &surface_context,
            &surface_size) == -1) {
        return -1;
    }
    (void)surface_context;
    if (rectangle.left < 0) {
        rectangle.left = 0;
    }
    if (rectangle.top < 0) {
        rectangle.top = 0;
    }
    if (rectangle.right > surface_size.cx) {
        rectangle.right = surface_size.cx;
    }
    if (rectangle.bottom > surface_size.cy) {
        rectangle.bottom = surface_size.cy;
    }
    if (rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return 0;
    }
    return boxrig_win32_renderer_present(
        component->renderer,
        device_context,
        &rectangle);
}

boxrig_ui_component_event_result
boxrig_win32_terminal_component_pretranslate_message(
    boxrig_win32_terminal_component *component,
    const MSG *message)
{
    int handled;

    if (component == NULL || message == NULL || component->input == NULL) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (!boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    handled = boxrig_win32_input_adapter_pretranslate_message(
        component->input,
        message);
    if (handled == -1) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    return handled == 1 ? BOXRIG_UI_COMPONENT_EVENT_HANDLED
                        : BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
}

boxrig_ui_component_event_result
boxrig_win32_terminal_component_dispatch_message(
    boxrig_win32_terminal_component *component,
    HWND target,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    LRESULT *result_out)
{
    LRESULT input_result = 0;
    int64_t pointer_x = 0;
    int64_t pointer_y = 0;
    int pointer_status;
    int scrollbar_status;
    int handled;
    bool input_blocked;

    if (component == NULL || result_out == NULL) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    *result_out = 0;
    if (!boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    if (target != component->input_window && target != component->scrollbar) {
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    if (target == component->input_window && message == WM_VSCROLL &&
        component->scrollbar_visibility.state.visible &&
        (long_parameter == 0 ||
         (HWND)long_parameter == component->scrollbar)) {
        return dispatch_vertical_scroll(
            component,
            word_parameter,
            result_out);
    }
    if (target != component->input_window) {
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    if ((message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED ||
         (message == WM_SHOWWINDOW && word_parameter == FALSE) ||
         (message == WM_SIZE && word_parameter == SIZE_MINIMIZED)) &&
        cancel_scrollbar_drag(component) == -1) return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    pointer_status = pointer_message_position(
        component,
        message,
        long_parameter,
        &pointer_x,
        &pointer_y);
    if (pointer_status == -1) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (pointer_status == 1 && component->scrollbar_visibility.state.pending)
        return BOXRIG_UI_COMPONENT_EVENT_BLOCKED;
    if (pointer_status == 1 && component->allocated) {
        unsigned int content_width = component_content_width(
            component,
            component->allocation.width);

        scrollbar_status = process_composited_scrollbar_pointer(
            component,
            message,
            word_parameter,
            pointer_x,
            pointer_y);
        if (scrollbar_status == -1) {
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        }
        if (scrollbar_status == 1) {
            if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                message == WM_XBUTTONDBLCLK) {
                *result_out = TRUE;
            }
            return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
        }
        if (pointer_x < 0 || pointer_y < 0 ||
            pointer_x >= content_width ||
            pointer_y >= component->allocation.height) {
            return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
        }
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        bool focused = message == WM_SETFOCUS;
        /* Visual focus is independent of terminal output backpressure. */
        boxrig_ui_status focus_status = boxrig_ui_view_set_focused(component->view, focused);
        if (focus_status != BOXRIG_UI_OK) {
            boxrig_win32_input_adapter_report_animation_failure(component->input, focus_status);
            return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
        }
        component->focused = focused;
    }
    boxrig_ui_input_state input_state;
    (void)boxrig_win32_input_adapter_get_input_state(component->input, &input_state);
    input_blocked = input_state.phase == BOXRIG_UI_INPUT_ENABLED &&
        boxrig_win32_input_adapter_blocked(component->input);
    handled = boxrig_win32_input_adapter_process_message(
        component->input,
        message,
        word_parameter,
        long_parameter,
        &input_result);
    if (handled == -1) {
        return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
    }
    if (handled == 1) {
        *result_out = input_result;
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    }
    if (input_blocked && message != WM_PAINT &&
        message != WM_ERASEBKGND && message != WM_SIZE) {
        return BOXRIG_UI_COMPONENT_EVENT_BLOCKED;
    }
    if (component->native_hosting == BOXRIG_UI_COMPONENT_BORROWED_TARGET &&
        (message == WM_ERASEBKGND || message == WM_PAINT ||
         message == WM_SIZE)) {
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
    switch (message) {
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    case WM_ERASEBKGND:
        *result_out = 1;
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    case WM_PAINT:
        {
            PAINTSTRUCT paint;
            HDC device_context = BeginPaint(component->window, &paint);
            bool failed = false;

            if (device_context == NULL) {
                return BOXRIG_UI_COMPONENT_EVENT_FAILURE;
            }
            failed = present_paint(
                         component,
                         device_context,
                         paint.rcPaint) == -1;
            EndPaint(component->window, &paint);
            return failed ? BOXRIG_UI_COMPONENT_EVENT_FAILURE
                          : BOXRIG_UI_COMPONENT_EVENT_HANDLED;
        }
    case WM_DISPLAYCHANGE:
        if (component->presentation != NULL) {
            component->presentation->invalidate(component->presentation_context);
            return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
        }
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    case WM_SIZE:
        return BOXRIG_UI_COMPONENT_EVENT_HANDLED;
    default:
        return BOXRIG_UI_COMPONENT_EVENT_UNRELATED;
    }
}

static LRESULT CALLBACK component_window_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    boxrig_win32_terminal_component *component =
        (boxrig_win32_terminal_component *)GetWindowLongPtrW(
            window,
            GWLP_USERDATA);
    LRESULT result = 0;
    boxrig_ui_component_event_result dispatch;

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        component = (boxrig_win32_terminal_component *)create->lpCreateParams;
        component->window = window;
        (void)SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            (LONG_PTR)component);
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    if (component == NULL) {
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    if (message == WM_NCDESTROY) {
        (void)boxrig_win32_terminal_component_quiesce(component);
        (void)boxrig_ui_view_set_focused(component->view, false);
        component->focused = false;
        component->visible = false;
        component->window = NULL;
        component->input_window = NULL;
        component->scrollbar = NULL;
        if (boxrig_ui_lifecycle_get_state(&component->lifecycle) ==
            BOXRIG_UI_LIFECYCLE_RUNNING) {
            (void)boxrig_ui_lifecycle_begin_shutdown(&component->lifecycle);
        }
        (void)boxrig_ui_lifecycle_mark_detached(&component->lifecycle);
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    if (component->input == NULL) {
        return DefWindowProcW(
            window,
            message,
            word_parameter,
            long_parameter);
    }
    dispatch = boxrig_win32_terminal_component_dispatch_message(
        component,
        window,
        message,
        word_parameter,
        long_parameter,
        &result);
    if (dispatch == BOXRIG_UI_COMPONENT_EVENT_HANDLED ||
        dispatch == BOXRIG_UI_COMPONENT_EVENT_BLOCKED ||
        dispatch == BOXRIG_UI_COMPONENT_EVENT_FAILURE) {
        return result;
    }
    return DefWindowProcW(
        window,
        message,
        word_parameter,
        long_parameter);
}

boxrig_ui_status boxrig_win32_terminal_component_frame(
    boxrig_win32_terminal_component *component,
    boxrig_ui_component_frame_result *result_out)
{
    boxrig_terminal *terminal;
    RECT rectangle;
    bool grid_resize_ready;
    bool waiting_for_grid_resize;
    bool waiting_for_allocation;
    bool presentation_deferred = false;
    int present_status;

    if (component == NULL || result_out == NULL || component->window == NULL ||
        component->input_window == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_status animation_status = boxrig_win32_input_adapter_animation_status(component->input);
    if (animation_status == BOXRIG_UI_OK) animation_status = boxrig_ui_view_animation_status(component->view);
    if (animation_status != BOXRIG_UI_OK) return animation_status;
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(boxrig_win32_input_adapter_composition(component->input),
            &composition) != BOXRIG_COMPOSITION_OK) return BOXRIG_UI_UNAVAILABLE;
    animation_status = boxrig_ui_view_set_composition_active(component->view,
        composition.active && composition.focused);
    if (animation_status == BOXRIG_UI_OK) animation_status = boxrig_ui_model_synchronize(component->model);
    if (animation_status != BOXRIG_UI_OK) return animation_status;
    terminal = boxrig_ui_model_terminal(component->model);
    if (terminal == NULL ||
        boxrig_win32_input_adapter_retry(component->input) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_status scrollbar_status = boxrig_win32_terminal_component_set_scrollbar_visibility(
        component, component->scrollbar_visibility.state.requested);
    if (scrollbar_status != BOXRIG_UI_OK && scrollbar_status != BOXRIG_UI_BUSY) return scrollbar_status;
    waiting_for_grid_resize = grid_resize_waiting_for_application(
        component, &grid_resize_ready);
    if (!waiting_for_grid_resize &&
        prepare_zoom_surface_candidate(component, &presentation_deferred) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    waiting_for_allocation = presentation_waits_for_allocation(component);
    if (!waiting_for_grid_resize && !waiting_for_allocation) {
        const int draw_status = component_update_view_composition(component,
                component->view,
                boxrig_win32_input_adapter_composition(component->input));
        if (draw_status < 0) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        presentation_deferred = presentation_deferred || draw_status > 0;
        component->ime_placement =
            boxrig_win32_input_adapter_synchronize_candidate(
                component->input);
        if (component->ime_placement == BOXRIG_WIN32_IME_PLACEMENT_ERROR ||
            synchronize_scrollbar(component) == -1) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        present_status = 0;
        while (component->presentation == NULL &&
               (present_status = boxrig_win32_renderer_take_present_dirty(
                    component->renderer,
                    &rectangle)) == 1) {
            if (component->presenter != NULL) {
                HDC surface;
                SIZE size;

                if (component_get_surface(component,
                        &surface,
                        &size) == -1 ||
                    component->presenter(
                        component->presenter_context,
                        surface,
                        size,
                        &rectangle) != BOXRIG_UI_OK) {
                    return BOXRIG_UI_UNAVAILABLE;
                }
            } else if (!InvalidateRect(
                           component->window,
                           &rectangle,
                           FALSE)) {
                return BOXRIG_UI_UNAVAILABLE;
            }
        }
        if (present_status == -1) {
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
        boxrig_win32_input_adapter_blocked(component->input);
    result_out->additional_dispatch =
        grid_resize_ready || result_out->input_blocked ||
        (!waiting_for_grid_resize &&
         (result_out->frame_pending ||
          surface_candidate_preparation_pending(component) ||
          size_command_ready_for_dispatch(component)));
    /* A minimized/occluded target can be dormant without an activity handle.
     * Retain its dirty, but dispatch only independent input/layout work. */
    if ((presentation_deferred || boxrig_win32_terminal_component_activity_handle(component) != NULL) &&
        !grid_resize_ready && !result_out->input_blocked &&
        !size_command_ready_for_dispatch(component))
        result_out->additional_dispatch = false;
    result_out->has_animation_deadline =
        boxrig_terminal_next_animation_deadline(
            terminal,
            &result_out->animation_deadline_milliseconds);
    uint64_t blink_deadline = 0;
    if (boxrig_ui_view_next_animation_deadline(component->view, &blink_deadline) &&
        (!result_out->has_animation_deadline || blink_deadline < result_out->animation_deadline_milliseconds)) {
        result_out->has_animation_deadline = true;
        result_out->animation_deadline_milliseconds = blink_deadline;
    }
    return BOXRIG_UI_OK;
}

boxrig_win32_ime_placement_status
boxrig_win32_terminal_component_resynchronize_ime_placement(
    boxrig_win32_terminal_component *component)
{
    if (component == NULL || component->input == NULL ||
        component->input_window == NULL ||
        !boxrig_ui_lifecycle_accepts_events(&component->lifecycle)) {
        return BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    }
    component->ime_placement =
        boxrig_win32_input_adapter_resynchronize_candidate(component->input);
    return component->ime_placement;
}

boxrig_win32_ime_placement_status
boxrig_win32_terminal_component_ime_placement_status(
    const boxrig_win32_terminal_component *component)
{
    return component == NULL ? BOXRIG_WIN32_IME_PLACEMENT_ERROR
                             : component->ime_placement;
}

boxrig_ui_status boxrig_win32_terminal_component_set_transparency(
    boxrig_win32_terminal_component *component,
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
    /* GPU presentation resolves alpha against its own target. The GDI font
     * provider does not need optional bitmap-alpha interfaces for glyph runs. */
    if (candidate.effective == BOXRIG_UI_TRANSPARENCY_UNAVAILABLE ||
        (component->presentation == NULL &&
         boxrig_win32_renderer_apply_transparency_state(
            component->renderer,
            &candidate) == -1)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_ui_view_set_transparency_config(component->view, config);
    if (status != BOXRIG_UI_OK) {
        if (component->presentation == NULL)
            (void)boxrig_win32_renderer_apply_transparency_state(
                component->renderer,
                &previous);
        return status;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_terminal_component_get_transparency(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_transparency_state *state_out)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_get_transparency(
                     component->view,
                     state_out);
}

uint32_t boxrig_win32_terminal_component_capabilities(
    const boxrig_win32_terminal_component *component)
{
    uint32_t capabilities;

    if (component == NULL) {
        return 0;
    }
    capabilities = BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY |
                   BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES |
                   BOXRIG_UI_COMPONENT_CAPABILITY_INPUT_METHOD |
                   BOXRIG_UI_COMPONENT_CAPABILITY_CLIPBOARD_PASTE |
                   BOXRIG_UI_COMPONENT_CAPABILITY_SIZE_COMMAND;
    if (component->presentation != NULL && component->presentation->transparency != NULL) {
        boxrig_ui_transparency_capabilities alpha = {0};
        component->presentation->transparency(component->presentation_context, &alpha);
        if (alpha.premultiplied_argb32_target)
            capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA;
    }
    if ((component->presentation == NULL &&
         boxrig_win32_renderer_supports_alpha(component->renderer))) {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA;
    }
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
    return capabilities;
}

boxrig_ui_status boxrig_win32_terminal_component_get_diagnostic(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_state transparency;

    if (component == NULL || diagnostic_out == NULL ||
        boxrig_win32_renderer_get_effective_metrics(
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
        (boxrig_ui_view_animation_status(component->view) != BOXRIG_UI_OK ||
         boxrig_win32_input_adapter_animation_status(component->input) != BOXRIG_UI_OK)) {
        diagnostic_out->running = false;
        diagnostic_out->failed = true;
        diagnostic_out->failure_reason = BOXRIG_UI_COMPONENT_FAILURE_ANIMATION;
    }
    diagnostic_out->visible = component->visible;
    diagnostic_out->focused = component->focused;
    diagnostic_out->input_blocked =
        boxrig_win32_input_adapter_blocked(component->input);
    diagnostic_out->layout_authority =
        boxrig_ui_view_is_layout_authority(component->view);
    diagnostic_out->layout_configured = component->allocated;
    diagnostic_out->metrics_revision = metrics.layout_revision;
    diagnostic_out->transparency_revision = transparency.revision;
    return BOXRIG_UI_OK;
}

void *boxrig_win32_terminal_component_presentation_context(
    boxrig_win32_terminal_component *component)
{
    return component == NULL ? NULL : component->presentation_context;
}

bool boxrig_win32_terminal_component_has_presentation(
    boxrig_win32_terminal_component *component,
    const boxrig_win32_component_presentation *presentation)
{
    return component != NULL && component->presentation == presentation;
}

bool boxrig_win32_terminal_component_set_renderer_phase_observer(
    boxrig_win32_terminal_component *component,
    const boxrig_win32_renderer_phase_observer *observer)
{
    if (component == NULL || component->presentation != NULL ||
        component->renderer == NULL) return false;
    boxrig_win32_renderer_set_phase_observer(component->renderer, observer);
    return true;
}

bool boxrig_win32_terminal_component_enable_renderer_work_statistics(
    boxrig_win32_terminal_component *component)
{
    if (component == NULL || component->presentation != NULL ||
        component->renderer == NULL) return false;
    boxrig_win32_renderer_reset_render_statistics(component->renderer);
    boxrig_win32_renderer_enable_work_statistics(component->renderer, true);
    return true;
}

bool boxrig_win32_terminal_component_get_renderer_work_statistics(
    const boxrig_win32_terminal_component *component,
    boxrig_win32_render_statistics *statistics_out)
{
    return component != NULL && component->presentation == NULL &&
        component->renderer != NULL && statistics_out != NULL &&
        boxrig_win32_renderer_get_render_statistics(
            component->renderer, statistics_out) == 0;
}

HANDLE boxrig_win32_terminal_component_activity_handle(
    boxrig_win32_terminal_component *component)
{
    return component == NULL || component->presentation == NULL ||
        component->presentation->activity == NULL ? NULL :
        component->presentation->activity(component->presentation_context);
}

boxrig_ui_status boxrig_win32_terminal_component_dispatch_activity(
    boxrig_win32_terminal_component *component)
{
    if (component == NULL || component->presentation == NULL ||
        component->presentation->dispatch_activity == NULL)
        return BOXRIG_UI_INVALID_ARGUMENT;
    component->presentation->dispatch_activity(component->presentation_context);
    return BOXRIG_UI_OK;
}
