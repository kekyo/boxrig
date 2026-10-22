/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "component_private.h"
#include "ibus_private.h"
#include "../ui/view_private.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_wayland_surface_regions {
    struct wl_region *input;
    struct wl_region *opaque;
} boxrig_wayland_surface_regions;

static void dirty_surface_for_buffer(
    boxrig_wayland_terminal_component *component,
    const boxrig_wayland_shm_buffer *buffer);

static bool component_uses_presentation_renderer(
    const boxrig_wayland_terminal_component *component)
{
    return component->presentation_renderer_operations != NULL;
}

/* The bar occupies fourteen surface-logical pixels, rounded up in the buffer. */
static unsigned int scrollbar_pixel_width(uint32_t scale_numerator)
{
    return (unsigned int)(((uint64_t)14 * scale_numerator + 119) / 120);
}

static unsigned int component_scrollbar_width(const boxrig_wayland_terminal_component *component)
{
    return component->scrollbar_committing ? component->scrollbar_visibility.target_width
        : component->scrollbar_visibility.state.reserved_width;
}

static unsigned int content_pixel_width(unsigned int width, unsigned int scrollbar_width)
{
    return width > scrollbar_width ? width - scrollbar_width : 1;
}

static int component_renderer_set_scrollbar(
    boxrig_wayland_terminal_component *component, bool enabled, unsigned int width)
{
    if (boxrig_pango_cairo_renderer_set_composited_scrollbar(component->renderer,
            enabled, width) == -1) return -1;
    return !component_uses_presentation_renderer(component) ? 0
        : component->presentation_renderer_operations->set_composited_scrollbar(
            component->presentation_renderer, enabled, width);
}

boxrig_ui_status boxrig_wayland_component_scrollbar_interaction(
    boxrig_wayland_terminal_component *component, bool hovered, bool dragging)
{
    if (component->scrollbar_hovered == hovered && component->scrollbar_dragging == dragging)
        return BOXRIG_UI_OK;
    if (boxrig_pango_cairo_renderer_set_composited_scrollbar_interaction(component->renderer,
            hovered, dragging) == -1 ||
        (component_uses_presentation_renderer(component) &&
         component->presentation_renderer_operations->set_composited_scrollbar_interaction(
            component->presentation_renderer, hovered, dragging) == -1)) return BOXRIG_UI_UNAVAILABLE;
    component->scrollbar_hovered = hovered;
    component->scrollbar_dragging = dragging;
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(component->terminal, &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

static uint64_t current_composition_revision(
    const boxrig_wayland_terminal_component *component)
{
    boxrig_composition_view composition;

    return boxrig_composition_get_view(component->composition, &composition) ==
                   BOXRIG_COMPOSITION_OK
               ? composition.revision : 0;
}

static bool component_frame_requested(
    const boxrig_wayland_terminal_component *component)
{
    /* Preedit may change while terminal cells and view dirty remain idle. */
    return boxrig_ui_view_frame_requested(component->view) ||
           current_composition_revision(component) !=
               component->rendered_composition_revision;
}

static int component_renderer_prepare_font(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    const boxrig_font_effective_metrics *metrics,
    boxrig_text_direction_policy direction_policy)
{
    boxrig_pango_cairo_renderer_font_selection selection;
    boxrig_wayland_component_font_presentation presentation;
    boxrig_font_catalog_status status;
    int result;

    if (!component_uses_presentation_renderer(component)) {
        return 0;
    }
    memset(&selection, 0, sizeof(selection));
    status = boxrig_pango_cairo_renderer_resolve_font_selection(
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
    presentation.direction_policy = direction_policy;
    result = component->presentation_renderer_operations->prepare_font(
        component->presentation_renderer,
        &presentation);
    boxrig_pango_cairo_renderer_font_selection_destroy(&selection);
    return result;
}

static int component_renderer_commit_font(
    boxrig_wayland_terminal_component *component)
{
    return !component_uses_presentation_renderer(component)
               ? 0
               : component->presentation_renderer_operations->commit_font(
                     component->presentation_renderer);
}

static int component_renderer_set_text_direction(
    boxrig_wayland_terminal_component *component,
    boxrig_text_direction_policy policy)
{
    return !component_uses_presentation_renderer(component)
               ? 0
               : component->presentation_renderer_operations
                     ->set_text_direction(
                         component->presentation_renderer,
                         policy);
}

static void component_renderer_abort_font(
    boxrig_wayland_terminal_component *component)
{
    if (component_uses_presentation_renderer(component)) {
        component->presentation_renderer_operations->abort_font(
            component->presentation_renderer);
    }
}

static int component_renderer_apply_transparency_state(
    boxrig_wayland_terminal_component *component,
    const boxrig_ui_transparency_state *state)
{
    if (boxrig_pango_cairo_renderer_apply_transparency_state(
            component->renderer,
            state) == -1) {
        return -1;
    }
    return !component_uses_presentation_renderer(component)
               ? 0
               : component->presentation_renderer_operations
                     ->apply_transparency_state(
                         component->presentation_renderer,
                         state);
}

static int component_renderer_prepare_surface_candidate(
    boxrig_wayland_terminal_component *component,
    unsigned int width,
    unsigned int height)
{
    return !component_uses_presentation_renderer(component)
               ? boxrig_pango_cairo_renderer_prepare_surface_candidate(
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
    boxrig_wayland_terminal_component *component,
    bool present)
{
    return !component_uses_presentation_renderer(component)
               ? boxrig_pango_cairo_renderer_commit_surface_candidate(
                     component->renderer)
               : component->presentation_renderer_operations
                     ->commit_surface_candidate(
                         component->presentation_renderer,
                         present);
}

static void component_renderer_abort_surface_candidate(
    boxrig_wayland_terminal_component *component)
{
    if (!component_uses_presentation_renderer(component)) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(
            component->renderer);
    } else {
        component->presentation_renderer_operations->abort_surface_candidate(
            component->presentation_renderer);
    }
}

static int component_renderer_draw_view_composition(
    boxrig_wayland_terminal_component *component,
    bool present)
{
    return !component_uses_presentation_renderer(component)
               ? boxrig_pango_cairo_renderer_draw_view_composition(
                     component->renderer,
                     component->view,
                     component->composition)
               : component->presentation_renderer_operations
                     ->draw_view_composition(
                         component->presentation_renderer,
                         component->view,
                         component->composition,
                         present);
}

static bool same_renderer_target(
    const boxrig_renderer_target *left,
    const boxrig_renderer_target *right)
{
    return left->width == right->width && left->height == right->height &&
           left->scale_numerator == right->scale_numerator &&
           left->scale_denominator == right->scale_denominator &&
           left->transparency_scopes == right->transparency_scopes &&
           left->premultiplied_alpha == right->premultiplied_alpha;
}

static boxrig_ui_status ensure_renderer_lifecycle(
    boxrig_wayland_terminal_component *component)
{
    boxrig_ui_transparency_state transparency;
    boxrig_renderer_target target;

    if (boxrig_ui_view_get_transparency(
            component->view,
            &transparency) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    memset(&target, 0, sizeof(target));
    target.width = component->buffer_width;
    target.height = component->buffer_height;
    target.scale_numerator = component->scale_numerator;
    target.scale_denominator = 120;
    target.transparency_scopes = transparency.effective_scopes;
    target.premultiplied_alpha =
        transparency.effective ==
        BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    if (component->renderer_lifecycle != NULL &&
        same_renderer_target(&component->renderer_target, &target)) {
        return BOXRIG_UI_OK;
    }
    boxrig_renderer_lifecycle_destroy(component->renderer_lifecycle);
    component->renderer_lifecycle = NULL;
    if (component->renderer_target.generation == UINT64_MAX) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    target.generation = component->renderer_target.generation + 1;
    if (boxrig_pango_cairo_renderer_lifecycle_create(
            component->renderer,
            &target,
            &component->renderer_lifecycle) != BOXRIG_RENDERER_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    component->renderer_target = target;
    return BOXRIG_UI_OK;
}

static bool valid_attachment(boxrig_ui_component_attachment attachment)
{
    return attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY ||
           attachment == BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
}

static bool valid_presentation_renderer(
    void *context,
    const boxrig_wayland_component_renderer_operations *operations)
{
    return context != NULL && operations != NULL &&
           operations->prepare_font != NULL &&
           operations->commit_font != NULL && operations->abort_font != NULL &&
           operations->set_text_direction != NULL &&
           operations->apply_transparency_state != NULL &&
           operations->set_composited_scrollbar != NULL &&
           operations->set_composited_scrollbar_interaction != NULL &&
           operations->prepare_surface_candidate != NULL &&
           operations->commit_surface_candidate != NULL &&
           operations->abort_surface_candidate != NULL &&
           operations->draw_view_composition != NULL &&
           operations->destroy != NULL;
}

static void font_specification_storage_destroy(
    boxrig_wayland_font_specification_storage *storage)
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
    boxrig_wayland_font_specification_storage *storage_out)
{
    boxrig_wayland_font_specification_storage storage;
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
            char *name;

            *destination = *source;
            if (source->name == NULL) {
                continue;
            }
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

static boxrig_wayland_output *find_output_by_proxy(
    boxrig_wayland_context *context,
    struct wl_output *proxy)
{
    size_t index;

    for (index = 0; index < context->output_count; index++) {
        if (context->outputs[index]->proxy == proxy) {
            return context->outputs[index];
        }
    }
    return NULL;
}

static boxrig_wayland_output *find_output_by_name(
    boxrig_wayland_context *context,
    uint32_t global_name)
{
    size_t index;

    for (index = 0; index < context->output_count; index++) {
        if (context->outputs[index]->global_name == global_name) {
            return context->outputs[index];
        }
    }
    return NULL;
}

static void recompute_scale_candidate(
    boxrig_wayland_terminal_component *component)
{
    uint32_t numerator = 120;
    size_t index;

    if (component->fractional_scale != NULL && component->viewport != NULL) {
        if (component->fractional_preferred_scale != 0) {
            numerator = component->fractional_preferred_scale;
        }
    } else if (component->preferred_buffer_scale != 0 &&
               component->preferred_buffer_scale <= UINT32_MAX / 120u) {
        numerator = component->preferred_buffer_scale * 120u;
    } else {
        for (index = 0; index < component->entered_output_count; index++) {
            boxrig_wayland_output *output = find_output_by_name(
                component->context,
                component->entered_outputs[index]);

            if (output != NULL && output->scale > 0 &&
                (uint32_t)output->scale <= UINT32_MAX / 120u &&
                (uint32_t)output->scale * 120u > numerator) {
                numerator = (uint32_t)output->scale * 120u;
            }
        }
    }
    component->candidate_scale_numerator = numerator;
    component->scale_dirty = numerator != component->scale_numerator;
}

static void queue_scale_recompute(
    boxrig_wayland_terminal_component *component)
{
    if (!component->running || component->scale_work_queued) {
        return;
    }
    if (boxrig_wayland_context_queue_component_scale(
            component->context,
            component)) {
        component->scale_work_queued = true;
    } else if (component->context->failure_reason ==
               BOXRIG_WAYLAND_FAILURE_WORK_QUEUE_OVERFLOW) {
        component->failure_reason =
            BOXRIG_WAYLAND_FAILURE_WORK_QUEUE_OVERFLOW;
    }
}

void boxrig_wayland_terminal_component_process_scale(
    boxrig_wayland_terminal_component *component)
{
    if (component == NULL) {
        return;
    }
    component->scale_work_queued = false;
    if (component->running) {
        recompute_scale_candidate(component);
    }
}

static void surface_enter(
    void *data,
    struct wl_surface *surface,
    struct wl_output *output)
{
    boxrig_wayland_terminal_component *component = data;
    boxrig_wayland_output *entry;
    uint32_t *outputs;
    size_t capacity;
    size_t index;

    (void)surface;
    entry = find_output_by_proxy(component->context, output);
    if (entry == NULL) {
        return;
    }
    for (index = 0; index < component->entered_output_count; index++) {
        if (component->entered_outputs[index] == entry->global_name) {
            return;
        }
    }
    if (component->entered_output_count == component->entered_output_capacity) {
        capacity = component->entered_output_capacity == 0
                       ? 4
                       : component->entered_output_capacity * 2;
        if (capacity > component->context->config.maximum_work_items) {
            capacity = component->context->config.maximum_work_items;
        }
        if (capacity <= component->entered_output_count ||
            capacity > SIZE_MAX / sizeof(*outputs)) {
            component->failure_reason = BOXRIG_WAYLAND_FAILURE_RESOURCE;
            return;
        }
        outputs = realloc(
            component->entered_outputs,
            capacity * sizeof(*outputs));
        if (outputs == NULL) {
            component->failure_reason = BOXRIG_WAYLAND_FAILURE_RESOURCE;
            return;
        }
        component->entered_outputs = outputs;
        component->entered_output_capacity = capacity;
    }
    component->entered_outputs[component->entered_output_count++] =
        entry->global_name;
    queue_scale_recompute(component);
}

static void surface_leave(
    void *data,
    struct wl_surface *surface,
    struct wl_output *output)
{
    boxrig_wayland_terminal_component *component = data;
    boxrig_wayland_output *entry;
    size_t index;

    (void)surface;
    entry = find_output_by_proxy(component->context, output);
    if (entry == NULL) {
        return;
    }
    for (index = 0; index < component->entered_output_count; index++) {
        if (component->entered_outputs[index] == entry->global_name) {
            component->entered_outputs[index] =
                component->entered_outputs[component->entered_output_count - 1];
            component->entered_output_count--;
            queue_scale_recompute(component);
            return;
        }
    }
}

static void surface_preferred_buffer_scale(
    void *data,
    struct wl_surface *surface,
    int32_t factor)
{
    boxrig_wayland_terminal_component *component = data;

    (void)surface;
    if (factor > 0) {
        component->preferred_buffer_scale = (uint32_t)factor;
        queue_scale_recompute(component);
    }
}

static void surface_preferred_buffer_transform(
    void *data,
    struct wl_surface *surface,
    uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

static void fractional_preferred_scale(
    void *data,
    struct wp_fractional_scale_v1 *fractional_scale,
    uint32_t scale)
{
    boxrig_wayland_terminal_component *component = data;

    (void)fractional_scale;
    if (scale != 0) {
        component->fractional_preferred_scale = scale;
        queue_scale_recompute(component);
    }
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_preferred_scale,
};

static void frame_done(
    void *data,
    struct wl_callback *callback,
    uint32_t callback_data)
{
    boxrig_wayland_terminal_component *component = data;

    (void)callback_data;
    if (component->frame_callback == callback) {
        wl_callback_destroy(callback);
        component->frame_callback = NULL;
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static boxrig_ui_status ensure_frame_callback(
    boxrig_wayland_terminal_component *component,
    bool *created_out)
{
    *created_out = false;
    if (component->frame_callback != NULL) {
        return BOXRIG_UI_OK;
    }
    component->frame_callback = wl_surface_frame(component->surface);
    if (component->frame_callback == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)component->frame_callback,
        component->context->queue);
    if (wl_callback_add_listener(
            component->frame_callback,
            &frame_listener,
            component) != 0) {
        wl_callback_destroy(component->frame_callback);
        component->frame_callback = NULL;
        return BOXRIG_UI_UNAVAILABLE;
    }
    *created_out = true;
    return BOXRIG_UI_OK;
}

static void destroy_surface_regions(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_surface_regions *regions)
{
    if (regions->opaque != NULL) {
        wl_region_destroy(regions->opaque);
        regions->opaque = NULL;
    }
    if (regions->input != NULL) {
        wl_region_destroy(regions->input);
        regions->input = NULL;
    }
    component->context->flush_required = true;
}

static boxrig_ui_status prepare_surface_regions(
    boxrig_wayland_terminal_component *component,
    unsigned int logical_width,
    unsigned int logical_height,
    bool opaque,
    boxrig_wayland_surface_regions *regions)
{
    memset(regions, 0, sizeof(*regions));

    regions->input =
        wl_compositor_create_region(component->context->compositor);
    if (regions->input == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)regions->input,
        component->context->queue);
    wl_region_add(
        regions->input,
        0,
        0,
        (int32_t)logical_width,
        (int32_t)logical_height);
    if (opaque) {
        regions->opaque = wl_compositor_create_region(
            component->context->compositor);
        if (regions->opaque == NULL) {
            destroy_surface_regions(component, regions);
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        wl_proxy_set_queue(
            (struct wl_proxy *)regions->opaque,
            component->context->queue);
        wl_region_add(
            regions->opaque,
            0,
            0,
            (int32_t)logical_width,
            (int32_t)logical_height);
    }
    component->context->flush_required = true;
    return BOXRIG_UI_OK;
}

static void commit_surface_regions(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_surface_regions *regions)
{
    wl_surface_set_input_region(component->surface, regions->input);
    wl_surface_set_opaque_region(component->surface, regions->opaque);
    destroy_surface_regions(component, regions);
}

static boxrig_ui_status apply_surface_regions(
    boxrig_wayland_terminal_component *component)
{
    boxrig_wayland_surface_regions regions;
    boxrig_ui_transparency_state transparency;
    bool opaque;

    if (component_uses_presentation_renderer(component)) {
        if (boxrig_ui_view_get_transparency(
                component->view,
                &transparency) != BOXRIG_UI_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        opaque = transparency.effective !=
                 BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    } else {
        opaque = component->pool != NULL &&
                 component->pool->format == WL_SHM_FORMAT_XRGB8888;
    }
    boxrig_ui_status status = prepare_surface_regions(
        component,
        component->logical_width,
        component->logical_height,
        opaque,
        &regions);

    if (status == BOXRIG_UI_OK) {
        commit_surface_regions(component, &regions);
    }
    return status;
}

static void unlink_component(boxrig_wayland_terminal_component *component)
{
    boxrig_wayland_terminal_component **link;

    link = &component->context->components;
    while (*link != NULL && *link != component) {
        link = &(*link)->next;
    }
    if (*link == component) {
        *link = component->next;
        component->context->component_count--;
    }
}

void boxrig_wayland_terminal_components_note_output(
    boxrig_wayland_context *context,
    uint32_t global_name)
{
    if (context == NULL || global_name == 0) {
        return;
    }
    boxrig_wayland_context_queue_output_change(context);
}

void boxrig_wayland_terminal_components_process_output_changes(
    boxrig_wayland_context *context)
{
    boxrig_wayland_terminal_component *component;

    if (context == NULL) {
        return;
    }
    for (component = context->components;
         component != NULL;
         component = component->next) {
        size_t index = 0;

        while (index < component->entered_output_count) {
            if (find_output_by_name(
                    context,
                    component->entered_outputs[index]) == NULL) {
                component->entered_outputs[index] = component->entered_outputs[
                    component->entered_output_count - 1];
                component->entered_output_count--;
            } else {
                index++;
            }
        }
        recompute_scale_candidate(component);
    }
}

static void schedule_parent_commit(
    boxrig_wayland_terminal_component *component)
{
    if ((component->parent_state_revision ==
             component->effective_parent_state_revision ||
         component->parent_action_taken) &&
        component->parent_state_revision != UINT64_MAX) {
        component->parent_state_revision++;
    }
    component->parent_action_taken = false;
    component->context->flush_required = true;
}

void boxrig_wayland_component_schedule_input_commit(
    boxrig_wayland_terminal_component *component, bool parent)
{
    if (parent) schedule_parent_commit(component);
    else component->presentation_pending = true;
    if (component->config.frame_request_callback != NULL)
        component->config.frame_request_callback(component->config.frame_request_context);
}

static bool schedule_position(
    boxrig_wayland_terminal_component *component,
    int32_t x,
    int32_t y)
{
    if (component->candidate_x == x && component->candidate_y == y) {
        return false;
    }
    component->candidate_x = x;
    component->candidate_y = y;
    if (component->parent_state_revision != UINT64_MAX) {
        component->parent_state_revision++;
    }
    component->parent_action_taken = false;
    wl_subsurface_set_position(component->subsurface, x, y);
    component->context->flush_required = true;
    return true;
}

boxrig_wayland_terminal_component_config
boxrig_wayland_terminal_component_config_default(void)
{
    boxrig_wayland_terminal_component_config config;

    memset(&config, 0, sizeof(config));
    config.view = boxrig_ui_view_config_default();
    config.attachment = BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    config.hosting = BOXRIG_UI_COMPONENT_OWNED_CHILD;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    config.buffer_count = BOXRIG_WAYLAND_BUFFERS_DEFAULT;
    config.maximum_bytes = BOXRIG_WAYLAND_BYTES_DEFAULT;
    config.maximum_dirty_rectangles =
        BOXRIG_WAYLAND_DIRTY_RECTS_DEFAULT;
    config.transparency = boxrig_ui_transparency_config_default();
    config.maximum_input_events = BOXRIG_WAYLAND_INPUT_EVENTS_DEFAULT;
    config.maximum_ime_bytes = BOXRIG_WAYLAND_IME_BYTES_DEFAULT;
    config.paste_capacity = BOXRIG_WAYLAND_PASTE_BYTES_DEFAULT;
    config.paste_chunk_size = BOXRIG_WAYLAND_PASTE_CHUNK_DEFAULT;
    config.multi_click_interval_milliseconds = 500;
    config.multi_click_distance = 4;
    return config;
}

boxrig_ui_status boxrig_wayland_terminal_component_create(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_terminal_component_config effective;
    boxrig_wayland_terminal_component *component;
    boxrig_wayland_terminal_component *existing;
    boxrig_font_specification default_specification;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_capabilities transparency;
    boxrig_ui_transparency_state transparency_state;
    boxrig_ui_status status;

    if (context == NULL || parent == NULL || model == NULL ||
        component_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *component_out = NULL;
    if (context->state == BOXRIG_WAYLAND_CONTEXT_INITIALIZING) {
        return BOXRIG_UI_BUSY;
    }
    if (context->state != BOXRIG_WAYLAND_CONTEXT_READY ||
        context->compositor == NULL || context->subcompositor == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (context->component_count >= context->config.maximum_components) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    effective = config == NULL
                    ? boxrig_wayland_terminal_component_config_default()
                    : *config;
    if (effective.keyboard_focus_fallback) {
        for (existing = context->components;
             existing != NULL;
             existing = existing->next) {
            if (existing->running &&
                existing->config.keyboard_focus_fallback) {
                return BOXRIG_UI_UNAVAILABLE;
            }
        }
    }
    if (!valid_attachment(effective.attachment) ||
        (effective.scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_NONE &&
         effective.scrollbar_presentation != BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED) ||
        effective.hosting != BOXRIG_UI_COMPONENT_OWNED_CHILD ||
        effective.buffer_count < BOXRIG_WAYLAND_BUFFERS_MINIMUM ||
        effective.buffer_count > BOXRIG_WAYLAND_BUFFERS_MAXIMUM ||
        effective.maximum_bytes == 0 ||
        effective.maximum_bytes > BOXRIG_WAYLAND_BYTES_MAXIMUM ||
        effective.maximum_dirty_rectangles == 0 ||
        effective.maximum_dirty_rectangles >
            BOXRIG_WAYLAND_DIRTY_RECTS_MAXIMUM ||
        effective.maximum_input_events == 0 ||
        effective.maximum_input_events > BOXRIG_WAYLAND_INPUT_EVENTS_MAXIMUM ||
        effective.maximum_ime_bytes == 0 ||
        effective.maximum_ime_bytes > BOXRIG_WAYLAND_IME_BYTES_MAXIMUM ||
        effective.paste_capacity == 0 ||
        effective.paste_capacity > BOXRIG_WAYLAND_PASTE_BYTES_MAXIMUM ||
        effective.paste_chunk_size == 0 ||
        effective.paste_chunk_size > BOXRIG_WAYLAND_PASTE_CHUNK_MAXIMUM ||
        effective.multi_click_interval_milliseconds == 0 ||
        effective.multi_click_distance == 0 ||
        (effective.frame_request_callback == NULL) !=
            (effective.frame_request_context == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (effective.animation_runtime == NULL &&
        (effective.view.blink.text_enabled || effective.view.blink.cursor_enabled)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    component->context = context;
    component->parent = parent;
    component->model = model;
    component->terminal = boxrig_ui_model_terminal(model);
    component->config = effective;
    default_specification = boxrig_font_specification_default();
    if (!font_specification_storage_copy(
            &default_specification,
            &component->font)) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    component->rendering_policy = boxrig_font_rendering_policy_default();
    component->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    if (component->terminal == NULL) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    if (boxrig_pango_cairo_renderer_create(
            1,
            1,
            &component->renderer) == -1) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    boxrig_ui_scrollbar_visibility_initialize(&component->scrollbar_visibility,
        effective.scrollbar_presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED, 14);
    if (component_renderer_set_scrollbar(component,
            component->scrollbar_visibility.state.visible, 14) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    if (boxrig_pango_cairo_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    status = boxrig_ui_view_create(model, &effective.view, &component->view);
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    if (effective.animation_runtime != NULL) {
        status = boxrig_ui_animation_runtime_attach(
            effective.animation_runtime, component->view);
        if (status != BOXRIG_UI_OK) goto failure;
    }
    status = boxrig_ui_view_require_animation_runtime(component->view);
    if (status != BOXRIG_UI_OK) goto failure;
    if (effective.attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
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
        goto failure;
    }
    memset(&transparency, 0, sizeof(transparency));
    transparency.premultiplied_argb32_target = context->argb8888;
    transparency.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    status = boxrig_ui_view_report_transparency_capabilities(
        component->view,
        &transparency);
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_set_transparency_config(
            component->view,
            &effective.transparency);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_get_transparency(
            component->view,
            &transparency_state);
    }
    if (status == BOXRIG_UI_OK &&
        boxrig_pango_cairo_renderer_apply_transparency_state(
            component->renderer,
            &transparency_state) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_set_geometry_capabilities(
            component->view,
            BOXRIG_UI_GEOMETRY_SIZE_COMMAND);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_set_frame_request_callback(
            component->view,
            effective.frame_request_callback,
            effective.frame_request_context);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_set_visible(component->view, false);
    }
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    component->surface = wl_compositor_create_surface(context->compositor);
    if (component->surface == NULL) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    wl_proxy_set_queue((struct wl_proxy *)component->surface, context->queue);
    if (wl_surface_add_listener(
            component->surface,
            &surface_listener,
            component) != 0) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    if (context->viewporter != NULL &&
        context->fractional_scale_manager != NULL) {
        component->viewport = wp_viewporter_get_viewport(
            context->viewporter,
            component->surface);
        component->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                context->fractional_scale_manager,
                component->surface);
        if (component->viewport == NULL || component->fractional_scale == NULL) {
            status = BOXRIG_UI_OUT_OF_MEMORY;
            goto failure;
        }
        wl_proxy_set_queue(
            (struct wl_proxy *)component->viewport,
            context->queue);
        wl_proxy_set_queue(
            (struct wl_proxy *)component->fractional_scale,
            context->queue);
        if (wp_fractional_scale_v1_add_listener(
                component->fractional_scale,
                &fractional_scale_listener,
                component) != 0) {
            status = BOXRIG_UI_UNAVAILABLE;
            goto failure;
        }
    }
    component->subsurface = wl_subcompositor_get_subsurface(
        context->subcompositor,
        component->surface,
        parent);
    if (component->subsurface == NULL) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)component->subsurface,
        context->queue);
    wl_subsurface_set_position(component->subsurface, 0, 0);
    wl_subsurface_set_desync(component->subsurface);
    context->flush_required = true;
    component->parent_state_revision = 1;
    component->scale_numerator = 120;
    component->candidate_scale_numerator = 120;
    component->scale_revision = 1;
    component->buffer_scale = 1;
    component->running = true;
    component->input_generation = 1;
    status = boxrig_wayland_input_initialize_component(component);
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    component->next = context->components;
    context->components = component;
    context->component_count++;
    *component_out = component;
    return BOXRIG_UI_OK;

failure:
    boxrig_wayland_terminal_component_destroy(component);
    return status;
}

boxrig_ui_status boxrig_wayland_terminal_component_adopt_renderer(
    boxrig_wayland_terminal_component *component,
    void *renderer_context,
    const boxrig_wayland_component_renderer_operations *renderer_operations,
    bool alpha_target,
    const boxrig_ui_transparency_config *transparency)
{
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state state;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_status status;

    if (component == NULL || !component->running ||
        component_uses_presentation_renderer(component) ||
        !valid_presentation_renderer(renderer_context, renderer_operations) ||
        transparency == NULL || component->allocated) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    component->presentation_renderer = renderer_context;
    component->presentation_renderer_operations = renderer_operations;
    component->presentation_alpha_target = alpha_target;
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target = alpha_target;
    if (alpha_target) {
        capabilities.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    status = boxrig_ui_view_report_transparency_capabilities(
        component->view,
        &capabilities);
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_set_transparency_config(
            component->view,
            transparency);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_get_transparency(component->view, &state);
    }
    if (status != BOXRIG_UI_OK ||
        component_renderer_set_scrollbar(component, component->scrollbar_visibility.state.visible,
            scrollbar_pixel_width(component->scale_numerator)) == -1 ||
        component_renderer_apply_transparency_state(component, &state) == -1 ||
        boxrig_pango_cairo_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        component_renderer_prepare_font(
            component,
            &component->font.specification,
            &component->rendering_policy,
            &metrics,
            component->direction_policy) == -1 ||
        component_renderer_commit_font(component) == -1) {
        component_renderer_abort_font(component);
        return status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
    }
    component->config.transparency = *transparency;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_quiesce(
    boxrig_wayland_terminal_component *component)
{
    if (component == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!component->running) {
        return BOXRIG_UI_OK;
    }
    component->running = false;
    boxrig_ui_scrollbar_visibility_cancel(&component->scrollbar_visibility);
    boxrig_ui_animation_runtime_detach(component->view);
    if (component->input_generation != UINT64_MAX) {
        component->input_generation++;
    }
    boxrig_wayland_clipboard_quiesce_component(component);
    boxrig_wayland_input_quiesce_component(component);
    if (component->view != NULL) {
        (void)boxrig_ui_view_set_frame_request_callback(
            component->view,
            NULL,
            NULL);
        (void)boxrig_ui_view_set_visible(component->view, false);
        (void)boxrig_ui_view_set_focused(component->view, false);
        boxrig_ui_view_release_layout_authority(component->view);
    }
    if (component->frame_callback != NULL) {
        wl_callback_destroy(component->frame_callback);
        component->frame_callback = NULL;
    }
    if (component->allocation_sync_pending && component->subsurface != NULL) {
        wl_subsurface_set_desync(component->subsurface);
        component->allocation_sync_pending = false;
        component->context->flush_required = true;
    }
    if (component->surface != NULL &&
        !component_uses_presentation_renderer(component)) {
        wl_surface_attach(component->surface, NULL, 0, 0);
        wl_surface_commit(component->surface);
        component->context->flush_required = true;
    }
    return BOXRIG_UI_OK;
}

void boxrig_wayland_terminal_component_destroy(
    boxrig_wayland_terminal_component *component)
{
    if (component == NULL) {
        return;
    }
    (void)boxrig_wayland_terminal_component_quiesce(component);
    if (component->context != NULL) {
        boxrig_wayland_context_remove_component_work(
            component->context,
            component);
        component->scale_work_queued = false;
    }
    boxrig_wayland_input_destroy_component(component);
    if (component->presentation_renderer_operations != NULL) {
        component->presentation_renderer_operations->destroy(
            component->presentation_renderer);
        component->presentation_renderer = NULL;
        component->presentation_renderer_operations = NULL;
        if (component->surface != NULL) {
            wl_surface_attach(component->surface, NULL, 0, 0);
            wl_surface_commit(component->surface);
        }
    }
    if (component->fractional_scale != NULL) {
        wp_fractional_scale_v1_destroy(component->fractional_scale);
    }
    if (component->viewport != NULL) {
        wp_viewport_destroy(component->viewport);
    }
    if (component->subsurface != NULL) {
        wl_subsurface_destroy(component->subsurface);
    }
    if (component->surface != NULL) {
        wl_surface_destroy(component->surface);
    }
    boxrig_wayland_shm_pool_destroy(component->pool);
    boxrig_font_visual_step_sequence_destroy(component->visual_steps);
    font_specification_storage_destroy(&component->font);
    boxrig_renderer_lifecycle_destroy(component->renderer_lifecycle);
    if (component->view != NULL) {
        boxrig_ui_view_destroy(component->view);
    }
    boxrig_pango_cairo_renderer_destroy(component->renderer);
    free(component->entered_outputs);
    if (component->context != NULL) {
        component->context->flush_required = true;
        unlink_component(component);
    }
    free(component);
}

struct wl_surface *boxrig_wayland_terminal_component_surface(
    boxrig_wayland_terminal_component *component)
{
    return component == NULL || !component->running ? NULL
                                                    : component->surface;
}

boxrig_ui_view *boxrig_wayland_terminal_component_view(
    boxrig_wayland_terminal_component *component)
{
    return component == NULL ? NULL : component->view;
}

boxrig_ui_status boxrig_wayland_terminal_component_get_scrollbar_state(
    boxrig_wayland_terminal_component *component, boxrig_scrollbar_state *state_out)
{
    return component == NULL ? BOXRIG_UI_INVALID_ARGUMENT
        : boxrig_ui_view_get_scrollbar_state(component->view, state_out);
}

boxrig_ui_status boxrig_wayland_terminal_component_get_scrollbar_model(
    boxrig_wayland_terminal_component *component, boxrig_scrollbar_model *model_out)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_scrollbar_model_read(boxrig_ui_model_terminal(component->model), model_out) == 0
        ? BOXRIG_UI_OK : BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_status boxrig_wayland_terminal_component_apply_scrollbar_action(
    boxrig_wayland_terminal_component *component, boxrig_scrollbar_action action,
    size_t position, bool *changed_out)
{
    if (component == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_ui_view_apply_scrollbar_action(component->view, action, position, changed_out);
}

boxrig_composition *boxrig_wayland_terminal_component_composition(
    boxrig_wayland_terminal_component *component)
{
    return component == NULL ? NULL : component->composition;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_input_config(
    boxrig_wayland_terminal_component *component, const boxrig_input_config *config)
{
    if (component == NULL || !component->running || config == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_composition_view composition;
    if (boxrig_composition_get_view(component->composition, &composition) !=
        BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->processing_input || component->input_control.state.phase == BOXRIG_UI_INPUT_STOPPING ||
        component->pressed_key_count != 0u || component->input_queue_depth != 0u ||
        component->input_blocked || component->ime_waiting ||
        composition.active || composition.commit_pending ||
        boxrig_wayland_clipboard_input_pending(component)) {
        return BOXRIG_UI_BUSY;
    }
    for (boxrig_wayland_seat *seat = component->context->seats;
         seat != NULL; seat = seat->next) {
        if ((seat->text_component == component &&
             (seat->pending_preedit_set || seat->pending_commit_set ||
              seat->pending_delete_set || seat->text_state_deferred)) ||
            (seat->keyboard_component == component && seat->compose_state != NULL &&
             xkb_compose_state_get_status(seat->compose_state) == XKB_COMPOSE_COMPOSING)) {
            return BOXRIG_UI_BUSY;
        }
    }
    boxrig_status status = boxrig_terminal_set_input_config(component->terminal, config);
    if (status == BOXRIG_STATUS_OK) return BOXRIG_UI_OK;
    if (status == BOXRIG_STATUS_OUT_OF_MEMORY) return BOXRIG_UI_OUT_OF_MEMORY;
    if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) return BOXRIG_UI_LIMIT_EXCEEDED;
    return BOXRIG_UI_INVALID_ARGUMENT;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_key_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_key_router router,
    void *context)
{
    if (component == NULL || (router == NULL && context != NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->pressed_key_count != 0) {
        return BOXRIG_UI_BUSY;
    }
    component->key_router = router;
    component->key_router_context = context;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_mouse_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_mouse_router router,
    void *context)
{
    if (component == NULL || (router == NULL && context != NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->pressed_button_count != 0) {
        return BOXRIG_UI_BUSY;
    }
    component->mouse_router = router;
    component->mouse_router_context = context;
    return BOXRIG_UI_OK;
}

boxrig_font_catalog_status
boxrig_wayland_terminal_component_get_effective_metrics(
    const boxrig_wayland_terminal_component *component,
    boxrig_font_effective_metrics *metrics_out)
{
    if (component == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_pango_cairo_renderer_get_effective_metrics(
        component->renderer,
        metrics_out);
}

static bool can_reuse_allocation(
    const boxrig_wayland_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    uint64_t width,
    uint64_t height)
{
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state command_state;

    /* A repeated native configure or a position-only change needs no new
     * image. Pending layout/paint work must still pass through the complete
     * allocation transaction, including font and scrollbar changes. */
    return component->allocated && !component->scrollbar_committing &&
           component->logical_width == allocation->width &&
           component->logical_height == allocation->height &&
           component->buffer_width == width &&
           component->buffer_height == height &&
           component->allocation.window_state == allocation->window_state &&
           !component->presentation_pending &&
           !component->layout_presentation_barrier &&
           component->dispatched_grid_resize.revision == 0 &&
           !component_frame_requested(component) &&
           !boxrig_ui_view_peek_layout_action(component->view, &action) &&
           boxrig_ui_view_get_size_command_state(component->view,
               &command_state) == BOXRIG_UI_OK &&
           command_state == BOXRIG_UI_SIZE_COMMAND_IDLE;
}

boxrig_ui_status boxrig_wayland_terminal_component_allocate(
    boxrig_wayland_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    boxrig_wayland_shm_pool *candidate = NULL;
    boxrig_wayland_shm_pool *previous;
    boxrig_wayland_surface_regions regions = {0};
    boxrig_wayland_shm_buffer *presentation_buffer;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_transparency_state transparency;
    cairo_surface_t *surface;
    uint64_t width;
    uint64_t height;
    uint64_t generation;
    uint32_t format;
    boxrig_ui_status status;
    bool callback_created = false;
    bool position_changed;
    bool external;

    if (component == NULL || allocation == NULL || layout_out == NULL ||
        !component->running || allocation->width == 0 ||
        allocation->height == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->scrollbar_visibility.state.pending && !component->scrollbar_committing)
        return BOXRIG_UI_BUSY;
    width = ((uint64_t)allocation->width * component->scale_numerator + 119) /
            120;
    height =
        ((uint64_t)allocation->height * component->scale_numerator + 119) /
        120;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (can_reuse_allocation(component, allocation, width, height)) {
        (void)schedule_position(component, allocation->x, allocation->y);
        component->allocation = *allocation;
        *layout_out = component->layout;
        return BOXRIG_UI_OK;
    }
    if (boxrig_pango_cairo_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_ui_view_get_transparency(
            component->view,
            &transparency) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    external = component_uses_presentation_renderer(component);
    format = transparency.effective ==
                     BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA
                 ? WL_SHM_FORMAT_ARGB8888
                 : WL_SHM_FORMAT_XRGB8888;
    if (!external) {
        if ((format == WL_SHM_FORMAT_ARGB8888 &&
             !component->context->argb8888) ||
            (format == WL_SHM_FORMAT_XRGB8888 &&
             !component->context->xrgb8888)) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        status = boxrig_wayland_shm_pool_create(
            component->context,
            component->config.buffer_count,
            (unsigned int)width,
            (unsigned int)height,
            format,
            component->config.maximum_bytes,
            component->config.maximum_dirty_rectangles,
            &candidate);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
    }
    if (component_renderer_prepare_surface_candidate(
            component,
            (unsigned int)width,
            (unsigned int)height) == -1) {
        boxrig_wayland_shm_pool_destroy(candidate);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    memset(&input, 0, sizeof(input));
    input.allocation_width =
        (boxrig_ui_fixed)content_pixel_width((unsigned int)width,
            component_scrollbar_width(component)) * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = allocation->window_state;
    status = boxrig_ui_view_accept_allocation(
        component->view,
        &input,
        layout_out);
    if (status != BOXRIG_UI_OK ||
        component_renderer_draw_view_composition(component, false) == -1) {
        component_renderer_abort_surface_candidate(component);
        boxrig_wayland_shm_pool_destroy(candidate);
        return status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
    }
    generation = component->presentation_generation == UINT64_MAX
                     ? UINT64_MAX
                     : component->presentation_generation + 1;
    status = BOXRIG_UI_OK;
    if (!external) {
        surface = boxrig_pango_cairo_renderer_surface(component->renderer);
        status = boxrig_wayland_shm_pool_copy_cairo(
            candidate,
            surface,
            generation);
    }
    if (status == BOXRIG_UI_OK && component->visible) {
        status = prepare_surface_regions(
            component,
            allocation->width,
            allocation->height,
            format == WL_SHM_FORMAT_XRGB8888,
            &regions);
    }
    if (status == BOXRIG_UI_OK && component->visible && external) {
        status = ensure_frame_callback(component, &callback_created);
        if (status == BOXRIG_UI_OK) {
            /* EGL's swap commits the pending Wayland surface state. */
            wl_subsurface_set_sync(component->subsurface);
            commit_surface_regions(component, &regions);
        }
    }
    if (status == BOXRIG_UI_OK && component->viewport != NULL) {
        /* The destination persists across buffer changes. Update it before
         * presentation so EGL's swap commits the matching logical extent. */
        wp_viewport_set_destination(
            component->viewport,
            (int32_t)allocation->width,
            (int32_t)allocation->height);
    }
    if (status != BOXRIG_UI_OK ||
        component_renderer_commit_surface_candidate(
            component,
            component->visible) == -1) {
        if (status == BOXRIG_UI_OK && component->viewport != NULL) {
            wp_viewport_set_destination(
                component->viewport,
                component->allocated ? (int32_t)component->logical_width : -1,
                component->allocated ? (int32_t)component->logical_height : -1);
        }
        if (callback_created) {
            wl_callback_destroy(component->frame_callback);
            component->frame_callback = NULL;
        }
        destroy_surface_regions(component, &regions);
        component_renderer_abort_surface_candidate(component);
        boxrig_wayland_shm_pool_destroy(candidate);
        return status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
    }
    previous = component->pool;
    component->pool = candidate;
    component->logical_width = allocation->width;
    component->logical_height = allocation->height;
    component->buffer_width = (unsigned int)width;
    component->buffer_height = (unsigned int)height;
    component->presentation_generation = generation;
    component->rendered_composition_revision =
        current_composition_revision(component);
    component->presentation_pending = true;
    component->allocation = *allocation;
    component->layout = *layout_out;
    component->allocated = true;
    position_changed = schedule_position(
        component,
        allocation->x,
        allocation->y);
    if (component->visible) {
        /* Cache the complete child allocation until the borrowed parent
         * publishes its matching root buffer. Normal animation remains
         * desynchronized after the host completes this transaction. */
        if (!external) {
            presentation_buffer = &component->pool->buffers[0];
            wl_subsurface_set_sync(component->subsurface);
            wl_surface_attach(
                component->surface,
                presentation_buffer->proxy,
                0,
                0);
            dirty_surface_for_buffer(component, presentation_buffer);
            commit_surface_regions(component, &regions);
            presentation_buffer->busy = true;
            wl_surface_commit(component->surface);
            boxrig_wayland_shm_buffer_clear_dirty(presentation_buffer);
        }
        component->presentation_pending = false;
        component->presentation_immediate = false;
        component->allocation_sync_pending = true;
        component->context->flush_required = true;
        if (!position_changed) {
            schedule_parent_commit(component);
        }
    }
    boxrig_wayland_shm_pool_destroy(previous);
    boxrig_wayland_input_update_cursor_rectangle(component);
    return BOXRIG_UI_OK;
}

static boxrig_ui_status commit_scrollbar_visibility(boxrig_wayland_terminal_component *component)
{
    if (!component->scrollbar_visibility.state.pending) return BOXRIG_UI_OK;
    if (component_renderer_set_scrollbar(component, component->scrollbar_visibility.target_visible,
            scrollbar_pixel_width(component->scale_numerator)) == -1) return BOXRIG_UI_UNAVAILABLE;
    component->scrollbar_committing = true;
    /* The transport has resized the core; refresh retained rows before drawing
     * the replacement buffer during the native allocation commit. */
    boxrig_ui_status status = boxrig_ui_model_synchronize(component->model);
    if (status == BOXRIG_UI_OK && component->allocated) {
        boxrig_ui_layout_result layout;
        status = boxrig_wayland_terminal_component_allocate(component, &component->allocation, &layout);
    }
    component->scrollbar_committing = false;
    if (status != BOXRIG_UI_OK) {
        (void)component_renderer_set_scrollbar(component, component->scrollbar_visibility.state.visible,
            scrollbar_pixel_width(component->scale_numerator));
        (void)boxrig_wayland_terminal_component_quiesce(component);
        return status;
    }
    boxrig_ui_scrollbar_visibility_commit(&component->scrollbar_visibility);
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(component->terminal, &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

boxrig_ui_status boxrig_wayland_terminal_component_set_scrollbar_visibility(
    boxrig_wayland_terminal_component *component, boxrig_ui_scrollbar_visibility policy)
{
    if (component == NULL || !component->running || policy < BOXRIG_UI_SCROLLBAR_ALWAYS ||
        policy > BOXRIG_UI_SCROLLBAR_NEVER) return BOXRIG_UI_INVALID_ARGUMENT;
    if (component->dispatched_grid_resize.revision != 0 || component->layout_presentation_barrier)
        return BOXRIG_UI_BUSY;
    boxrig_ui_status status = boxrig_ui_model_synchronize(component->model);
    if (status != BOXRIG_UI_OK) return status;
    boxrig_ui_allocation_input input = {0};
    if (component->allocated) {
        boxrig_font_effective_metrics metrics;
        if (boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) != BOXRIG_FONT_CATALOG_OK)
            return BOXRIG_UI_UNAVAILABLE;
        input.allocation_width = (boxrig_ui_fixed)component->buffer_width * BOXRIG_UI_FIXED_ONE;
        input.allocation_height = (boxrig_ui_fixed)component->buffer_height * BOXRIG_UI_FIXED_ONE;
        input.cell_width = (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
        input.cell_height = (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
        input.cell_metrics_revision = metrics.layout_revision;
        input.window_state = component->allocation.window_state;
    }
    status = boxrig_ui_scrollbar_visibility_prepare(&component->scrollbar_visibility, component->view,
        policy, component->config.scrollbar_presentation == BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED,
        scrollbar_pixel_width(component->scale_numerator), component->allocated ? &input : NULL);
    if (status != BOXRIG_UI_OK || !component->scrollbar_visibility.state.pending) return status;
    boxrig_wayland_input_cancel_scrollbar(component, NULL);
    boxrig_ui_layout_action action;
    if (!boxrig_ui_view_peek_layout_action(component->view, &action))
        return commit_scrollbar_visibility(component);
    component->layout_presentation_barrier = true;
    boxrig_ui_dirty dirty = {0};
    boxrig_terminal_size(component->terminal, &dirty.row_count, &dirty.column_count);
    return boxrig_ui_view_invalidate(component->view, BOXRIG_UI_DIRTY_PRESENT, dirty);
}

boxrig_ui_status boxrig_wayland_terminal_component_get_scrollbar_visibility(
    const boxrig_wayland_terminal_component *component, boxrig_ui_scrollbar_visibility_state *state_out)
{
    if (component == NULL || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = component->scrollbar_visibility.state;
    state_out->reserved_width = state_out->visible ? 14 : 0;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_get_layout(
    const boxrig_wayland_terminal_component *component,
    boxrig_ui_component_allocation *allocation_out,
    boxrig_ui_layout_result *layout_out)
{
    if (component == NULL || allocation_out == NULL || layout_out == NULL ||
        !component->allocated) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *allocation_out = component->allocation;
    *layout_out = component->layout;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_visible(
    boxrig_wayland_terminal_component *component,
    bool visible)
{
    if (component == NULL || !component->running) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_status status = boxrig_ui_view_set_visible(
        component->view,
        visible);

    if (status == BOXRIG_UI_OK) {
        component->visible = visible;
        if (!visible) boxrig_wayland_input_cancel_scrollbar(component, NULL);
    }
    return status;
}

static boxrig_ui_status font_status_to_ui(boxrig_font_catalog_status status)
{
    if (status == BOXRIG_FONT_CATALOG_OUT_OF_MEMORY) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (status == BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    return status == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT
               ? BOXRIG_UI_INVALID_ARGUMENT
               : BOXRIG_UI_UNAVAILABLE;
}

static boxrig_ui_status reconfigure_layout_for_metrics(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_effective_metrics *metrics,
    boxrig_ui_layout_change_kind kind)
{
    boxrig_ui_layout_change_input input;
    boxrig_ui_layout_action action;
    boxrig_ui_layout_result layout;
    boxrig_ui_status status;

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
    status = boxrig_ui_view_apply_layout_change(
        component->view,
        &input,
        &layout);
    if (status == BOXRIG_UI_OK) {
        component->layout = layout;
        if (boxrig_ui_view_peek_layout_action(component->view, &action)) {
            component->layout_presentation_barrier = true;
        }
    }
    return status;
}

static boxrig_ui_status set_component_font(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_ui_layout_change_kind change_kind)
{
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    boxrig_wayland_font_specification_storage storage;
    boxrig_font_effective_metrics metrics;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (component == NULL || !component->running || specification == NULL ||
        policy == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    memset(&storage, 0, sizeof(storage));
    font_status = boxrig_pango_cairo_renderer_prepare_font(
        component->renderer,
        specification,
        policy,
        &candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return font_status_to_ui(font_status);
    }
    font_status = boxrig_pango_cairo_renderer_font_candidate_metrics(
        candidate,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return font_status_to_ui(font_status);
    }
    status = boxrig_ui_view_validate_cell_metrics(component->view, &metrics);
    if (status != BOXRIG_UI_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return status;
    }
    if (!font_specification_storage_copy(specification, &storage)) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (component_renderer_prepare_font(
            component,
            specification,
            policy,
            &metrics,
            component->direction_policy) == -1) {
        font_specification_storage_destroy(&storage);
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->config.attachment ==
        BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
        status = boxrig_ui_view_commit_cell_metrics(
            component->view,
            &metrics);
        if (status != BOXRIG_UI_OK) {
            component_renderer_abort_font(component);
            font_specification_storage_destroy(&storage);
            boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
            return status;
        }
    }
    font_status = boxrig_pango_cairo_renderer_commit_font(
        component->renderer,
        candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        component_renderer_abort_font(component);
        font_specification_storage_destroy(&storage);
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return font_status_to_ui(font_status);
    }
    if (component_renderer_commit_font(component) == -1) {
        component_renderer_abort_font(component);
        font_specification_storage_destroy(&storage);
        return BOXRIG_UI_UNAVAILABLE;
    }
    font_specification_storage_destroy(&component->font);
    component->font = storage;
    component->rendering_policy = *policy;
    boxrig_font_visual_step_sequence_destroy(component->visual_steps);
    component->visual_steps = NULL;
    component->visual_step_policy_valid = false;
    return reconfigure_layout_for_metrics(
        component,
        &metrics,
        change_kind);
}

boxrig_ui_status boxrig_wayland_terminal_component_set_font_specification(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_specification *specification)
{
    return component == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : set_component_font(
                     component,
                     specification,
                     &component->rendering_policy,
                     BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS);
}

boxrig_ui_status boxrig_wayland_terminal_component_get_font_specification(
    const boxrig_wayland_terminal_component *component,
    boxrig_font_specification *specification_out)
{
    if (component == NULL || specification_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *specification_out = component->font.specification;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_font_rendering_policy(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_rendering_policy *policy)
{
    if (component == NULL || policy == NULL ||
        policy->output_scale !=
            (float)component->scale_numerator / 120.0f) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return set_component_font(
        component,
        &component->font.specification,
        policy,
        BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS);
}

boxrig_ui_status boxrig_wayland_terminal_component_get_font_rendering_policy(
    const boxrig_wayland_terminal_component *component,
    boxrig_font_rendering_policy *policy_out)
{
    if (component == NULL || policy_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *policy_out = component->rendering_policy;
    return BOXRIG_UI_OK;
}

boxrig_font_catalog_status
boxrig_wayland_terminal_component_register_font_file(
    boxrig_wayland_terminal_component *component,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (component == NULL || !component->running) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_pango_cairo_renderer_register_font_file(
        component->renderer,
        path,
        path_length,
        face_index,
        resource_id_out);
}

boxrig_font_catalog_status
boxrig_wayland_terminal_component_register_font_memory(
    boxrig_wayland_terminal_component *component,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (component == NULL || !component->running) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_pango_cairo_renderer_register_font_memory(
        component->renderer,
        data,
        data_length,
        face_index,
        resource_id_out);
}

boxrig_font_resource_id
boxrig_wayland_terminal_component_primary_font_resource(
    const boxrig_wayland_terminal_component *component)
{
    return boxrig_pango_cairo_renderer_primary_font_resource(
        component == NULL ? NULL : component->renderer);
}

uint64_t boxrig_wayland_terminal_component_font_catalog_revision(
    const boxrig_wayland_terminal_component *component)
{
    return boxrig_pango_cairo_renderer_font_catalog_revision(
        component == NULL ? NULL : component->renderer);
}

boxrig_ui_status
boxrig_wayland_terminal_component_set_line_height_adjustment(
    boxrig_wayland_terminal_component *component,
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
        BOXRIG_UI_LAYOUT_CHANGE_LINE_HEIGHT);
}

boxrig_ui_status
boxrig_wayland_terminal_component_get_line_height_adjustment(
    const boxrig_wayland_terminal_component *component,
    float *adjustment_dip_out)
{
    if (component == NULL || adjustment_dip_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *adjustment_dip_out =
        component->font.specification.line_height_adjustment_dip;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_text_direction(
    boxrig_wayland_terminal_component *component,
    boxrig_text_direction_policy policy)
{
    boxrig_ui_dirty dirty;

    if (component == NULL || !component->running) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->direction_policy == policy) {
        return BOXRIG_UI_OK;
    }
    if (boxrig_pango_cairo_renderer_set_direction_policy(
            component->renderer,
            policy) == -1 ||
        component_renderer_set_text_direction(component, policy) == -1) {
        (void)boxrig_pango_cairo_renderer_set_direction_policy(
            component->renderer,
            component->direction_policy);
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    component->direction_policy = policy;
    if (!component->allocated) {
        return BOXRIG_UI_OK;
    }
    dirty.row = 0;
    dirty.row_count = component->layout.rows;
    dirty.column = 0;
    dirty.column_count = component->layout.columns;
    return boxrig_ui_view_invalidate(
        component->view,
        BOXRIG_UI_DIRTY_PRESENT,
        dirty);
}

boxrig_text_direction_policy
boxrig_wayland_terminal_component_text_direction(
    const boxrig_wayland_terminal_component *component)
{
    return component == NULL ? BOXRIG_TEXT_DIRECTION_AUTO
                             : component->direction_policy;
}

static bool visual_step_policy_matches(
    const boxrig_wayland_terminal_component *component,
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
    boxrig_wayland_terminal_component *component = context;
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    boxrig_font_specification specification;
    boxrig_font_catalog_status status;

    if (component == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    specification = component->font.specification;
    specification.size_dip = requested_size_dip;
    specification.line_height_adjustment_dip = 0.0f;
    status = boxrig_pango_cairo_renderer_prepare_font(
        component->renderer,
        &specification,
        &component->rendering_policy,
        &candidate);
    if (status == BOXRIG_FONT_CATALOG_OK) {
        status = boxrig_pango_cairo_renderer_font_candidate_metrics(
            candidate,
            metrics_out);
    }
    boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
    return status;
}

static boxrig_ui_status commit_component_zoom(
    boxrig_wayland_terminal_component *component,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    boxrig_font_specification specification = component->font.specification;
    boxrig_font_effective_metrics prepared_metrics;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    specification.size_dip = metrics->requested_size_dip;
    font_status = boxrig_pango_cairo_renderer_prepare_font(
        component->renderer,
        &specification,
        &component->rendering_policy,
        &candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return font_status_to_ui(font_status);
    }
    font_status = boxrig_pango_cairo_renderer_font_candidate_metrics(
        candidate,
        &prepared_metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return font_status_to_ui(font_status);
    }
    status = boxrig_ui_view_validate_cell_metrics(
        component->view,
        &prepared_metrics);
    if (status != BOXRIG_UI_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return status;
    }
    if (component_renderer_prepare_font(
            component,
            &specification,
            &component->rendering_policy,
            &prepared_metrics,
            component->direction_policy) == -1) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (component->config.attachment ==
        BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
        status = boxrig_ui_view_commit_cell_metrics(
            component->view,
            &prepared_metrics);
        if (status != BOXRIG_UI_OK) {
            component_renderer_abort_font(component);
            boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
            return status;
        }
    }
    font_status = boxrig_pango_cairo_renderer_commit_font(
        component->renderer,
        candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        component_renderer_abort_font(component);
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return font_status_to_ui(font_status);
    }
    if (component_renderer_commit_font(component) == -1) {
        component_renderer_abort_font(component);
        return BOXRIG_UI_UNAVAILABLE;
    }
    return reconfigure_layout_for_metrics(
        component,
        &prepared_metrics,
        BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS);
}

boxrig_ui_status boxrig_wayland_terminal_component_visual_zoom(
    boxrig_wayland_terminal_component *component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    boxrig_font_visual_step_sequence *sequence = NULL;
    boxrig_font_effective_metrics current;
    boxrig_font_effective_metrics candidate;
    boxrig_font_catalog_status font_status;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_status status;

    if (component == NULL || changed_out == NULL || !component->running ||
        (direction != BOXRIG_FONT_ZOOM_DECREASE &&
         direction != BOXRIG_FONT_ZOOM_INCREASE)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    if (boxrig_ui_view_get_size_command_state(
            component->view,
            &command_state) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED) {
        return BOXRIG_UI_BUSY;
    }
    if (component->visual_steps == NULL ||
        !visual_step_policy_matches(component, policy)) {
        font_status = boxrig_pango_cairo_renderer_get_effective_metrics(
            component->renderer,
            &current);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return font_status_to_ui(font_status);
        }
        font_status = boxrig_font_visual_step_sequence_create(
            &current,
            component->font.specification.line_height_adjustment_dip,
            policy,
            resolve_component_zoom_metrics,
            component,
            &sequence);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return font_status_to_ui(font_status);
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
    status = commit_component_zoom(component, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_advance(
        component->visual_steps,
        direction);
    *changed_out = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_visual_zoom_reset(
    boxrig_wayland_terminal_component *component,
    bool *changed_out)
{
    boxrig_font_effective_metrics candidate;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_status status;

    if (component == NULL || changed_out == NULL || !component->running) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (component->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    if (boxrig_ui_view_get_size_command_state(
            component->view,
            &command_state) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED) {
        return BOXRIG_UI_BUSY;
    }
    if (!boxrig_font_visual_step_sequence_peek_reset(
            component->visual_steps,
            &candidate)) {
        return BOXRIG_UI_OK;
    }
    status = commit_component_zoom(component, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_reset(component->visual_steps);
    *changed_out = true;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status apply_pending_scale_opengl(
    boxrig_wayland_terminal_component *component)
{
    boxrig_pango_cairo_renderer_font_candidate *font_candidate = NULL;
    boxrig_font_specification specification = component->font.specification;
    boxrig_font_rendering_policy policy = component->rendering_policy;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_action previous_action;
    boxrig_ui_layout_result layout;
    boxrig_font_catalog_status font_status;
    uint64_t width;
    uint64_t height;
    uint64_t generation;
    uint64_t previous_grid_revision = 0;
    uint32_t numerator = component->candidate_scale_numerator;
    int32_t buffer_scale;
    bool fractional;
    bool surface_candidate = false;
    boxrig_ui_status status;

    if (numerator == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    width = ((uint64_t)component->logical_width * numerator + 119) / 120;
    height = ((uint64_t)component->logical_height * numerator + 119) / 120;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    fractional = component->fractional_scale != NULL &&
                 component->viewport != NULL;
    buffer_scale = fractional ? 1 : (int32_t)(numerator / 120u);
    if (buffer_scale <= 0 ||
        (!fractional && (uint32_t)buffer_scale * 120u != numerator) ||
        (buffer_scale != 1 &&
         wl_proxy_get_version((struct wl_proxy *)component->surface) <
             WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    policy.output_scale = (float)numerator / 120.0f;
    font_status = boxrig_pango_cairo_renderer_prepare_font(
        component->renderer,
        &specification,
        &policy,
        &font_candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return font_status_to_ui(font_status);
    }
    font_status = boxrig_pango_cairo_renderer_font_candidate_metrics(
        font_candidate,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        status = font_status_to_ui(font_status);
        goto failure;
    }
    status = boxrig_ui_view_validate_cell_metrics(component->view, &metrics);
    if (status != BOXRIG_UI_OK ||
        component_renderer_prepare_font(
            component,
            &specification,
            &policy,
            &metrics,
            component->direction_policy) == -1) {
        status = status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
        goto failure;
    }
    if (component->config.attachment ==
        BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
        status = boxrig_ui_view_commit_cell_metrics(component->view, &metrics);
        if (status != BOXRIG_UI_OK) {
            goto failure;
        }
    }
    font_status = boxrig_pango_cairo_renderer_commit_font(
        component->renderer,
        font_candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        status = font_status_to_ui(font_status);
        goto failure;
    }
    font_candidate = NULL;
    if (component_renderer_commit_font(component) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    if (component_renderer_prepare_surface_candidate(
            component,
            (unsigned int)width,
            (unsigned int)height) == -1) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    surface_candidate = true;
    memset(&input, 0, sizeof(input));
    unsigned int bar_width = component->scrollbar_visibility.state.visible
        ? scrollbar_pixel_width(numerator) : 0;
    if (component_renderer_set_scrollbar(component, bar_width != 0, scrollbar_pixel_width(numerator)) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    input.allocation_width =
        (boxrig_ui_fixed)content_pixel_width((unsigned int)width, bar_width) * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = component->allocation.window_state;
    if (boxrig_ui_view_peek_layout_action(
            component->view, &previous_action) &&
        previous_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        previous_grid_revision = previous_action.revision;
    }
    status = boxrig_ui_view_accept_allocation(component->view, &input, &layout);
    if (status != BOXRIG_UI_OK ||
        component_renderer_draw_view_composition(component, false) == -1) {
        status = status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
        goto failure;
    }
    {
        boxrig_ui_layout_action action;

        if (boxrig_ui_view_peek_layout_action(component->view, &action) &&
            action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE &&
            action.revision != previous_grid_revision) {
            component->layout_presentation_barrier = true;
        }
    }
    if (component->visible) {
        status = apply_surface_regions(component);
        if (status != BOXRIG_UI_OK) {
            goto failure;
        }
    }
    if (wl_proxy_get_version((struct wl_proxy *)component->surface) >=
        WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_set_buffer_scale(component->surface, buffer_scale);
    }
    if (component->viewport != NULL) {
        wp_viewport_set_destination(
            component->viewport,
            (int32_t)component->logical_width,
            (int32_t)component->logical_height);
    }
    if (component_renderer_commit_surface_candidate(component, false) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    surface_candidate = false;
    generation = component->presentation_generation == UINT64_MAX
                     ? UINT64_MAX
                     : component->presentation_generation + 1;
    component->buffer_width = (unsigned int)width;
    component->buffer_height = (unsigned int)height;
    component->layout = layout;
    component->presentation_generation = generation;
    component->rendered_composition_revision =
        current_composition_revision(component);
    component->presentation_pending = true;
    component->presentation_immediate = false;
    boxrig_wayland_input_cancel_scrollbar(component, NULL);
    component->scrollbar_visibility.state.reserved_width = bar_width;
    component->scrollbar_visibility.target_width = bar_width;
    component->scale_numerator = numerator;
    component->buffer_scale = buffer_scale;
    component->rendering_policy = policy;
    boxrig_font_visual_step_sequence_destroy(component->visual_steps);
    component->visual_steps = NULL;
    component->visual_step_policy_valid = false;
    if (component->scale_revision != UINT64_MAX) {
        component->scale_revision++;
    }
    component->scale_dirty = false;
    component->context->flush_required = true;
    boxrig_wayland_input_update_cursor_rectangle(component);
    return BOXRIG_UI_OK;

failure:
    (void)component_renderer_set_scrollbar(component, component->scrollbar_visibility.state.visible,
        scrollbar_pixel_width(component->scale_numerator));
    if (surface_candidate) {
        component_renderer_abort_surface_candidate(component);
    }
    component_renderer_abort_font(component);
    boxrig_pango_cairo_renderer_font_candidate_destroy(font_candidate);
    return status;
}

static boxrig_ui_status apply_pending_scale(
    boxrig_wayland_terminal_component *component)
{
    boxrig_pango_cairo_renderer_font_candidate *font_candidate = NULL;
    boxrig_wayland_shm_pool *pool_candidate = NULL;
    boxrig_wayland_shm_pool *previous_pool;
    boxrig_font_specification specification =
        component->font.specification;
    boxrig_font_rendering_policy policy = component->rendering_policy;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_action previous_action;
    boxrig_ui_layout_result layout;
    boxrig_ui_transparency_state transparency;
    boxrig_font_catalog_status font_status;
    cairo_surface_t *surface;
    uint64_t width;
    uint64_t height;
    uint64_t generation;
    uint64_t previous_grid_revision = 0;
    uint32_t numerator = component->candidate_scale_numerator;
    uint32_t format;
    int32_t buffer_scale;
    bool fractional;
    boxrig_ui_status status;

    if (!component->scale_dirty || numerator == component->scale_numerator) {
        component->scale_dirty = false;
        return BOXRIG_UI_OK;
    }
    if (component_uses_presentation_renderer(component)) {
        return apply_pending_scale_opengl(component);
    }
    if (numerator == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    width = ((uint64_t)component->logical_width * numerator + 119) / 120;
    height = ((uint64_t)component->logical_height * numerator + 119) / 120;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    fractional = component->fractional_scale != NULL &&
                 component->viewport != NULL;
    buffer_scale = fractional ? 1 : (int32_t)(numerator / 120u);
    if (buffer_scale <= 0 ||
        (!fractional && (uint32_t)buffer_scale * 120u != numerator) ||
        (buffer_scale != 1 &&
         wl_proxy_get_version((struct wl_proxy *)component->surface) <
             WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    policy.output_scale = (float)numerator / 120.0f;
    font_status = boxrig_pango_cairo_renderer_prepare_font(
        component->renderer,
        &specification,
        &policy,
        &font_candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return font_status_to_ui(font_status);
    }
    font_status = boxrig_pango_cairo_renderer_font_candidate_metrics(
        font_candidate,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        status = font_status_to_ui(font_status);
        goto failure;
    }
    status = boxrig_ui_view_validate_cell_metrics(component->view, &metrics);
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    if (boxrig_ui_view_get_transparency(
            component->view,
            &transparency) != BOXRIG_UI_OK) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    format = transparency.effective ==
                     BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA
                 ? WL_SHM_FORMAT_ARGB8888
                 : WL_SHM_FORMAT_XRGB8888;
    status = boxrig_wayland_shm_pool_create(
        component->context,
        component->config.buffer_count,
        (unsigned int)width,
        (unsigned int)height,
        format,
        component->config.maximum_bytes,
        component->config.maximum_dirty_rectangles,
        &pool_candidate);
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    if (boxrig_pango_cairo_renderer_prepare_surface_candidate(
            component->renderer,
            (unsigned int)width,
            (unsigned int)height) == -1) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    if (component->config.attachment ==
        BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
        status = boxrig_ui_view_commit_cell_metrics(
            component->view,
            &metrics);
        if (status != BOXRIG_UI_OK) {
            goto failure;
        }
    }
    font_status = boxrig_pango_cairo_renderer_commit_font(
        component->renderer,
        font_candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        status = font_status_to_ui(font_status);
        goto failure;
    }
    font_candidate = NULL;
    memset(&input, 0, sizeof(input));
    unsigned int bar_width = component->scrollbar_visibility.state.visible
        ? scrollbar_pixel_width(numerator) : 0;
    if (component_renderer_set_scrollbar(component, bar_width != 0, scrollbar_pixel_width(numerator)) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    input.allocation_width =
        (boxrig_ui_fixed)content_pixel_width((unsigned int)width, bar_width) * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = component->allocation.window_state;
    if (boxrig_ui_view_peek_layout_action(
            component->view, &previous_action) &&
        previous_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        previous_grid_revision = previous_action.revision;
    }
    status = boxrig_ui_view_accept_allocation(
        component->view,
        &input,
        &layout);
    if (status != BOXRIG_UI_OK ||
        boxrig_pango_cairo_renderer_draw_view_composition(
            component->renderer,
            component->view,
            component->composition) == -1) {
        status = status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
        goto failure;
    }
    {
        boxrig_ui_layout_action action;

        if (boxrig_ui_view_peek_layout_action(component->view, &action) &&
            action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE &&
            action.revision != previous_grid_revision) {
            component->layout_presentation_barrier = true;
        }
    }
    generation = component->presentation_generation == UINT64_MAX
                     ? UINT64_MAX
                     : component->presentation_generation + 1;
    surface = boxrig_pango_cairo_renderer_surface(component->renderer);
    status = boxrig_wayland_shm_pool_copy_cairo(
        pool_candidate,
        surface,
        generation);
    if (status != BOXRIG_UI_OK ||
        boxrig_pango_cairo_renderer_commit_surface_candidate(
            component->renderer) == -1) {
        status = status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
        goto failure;
    }
    previous_pool = component->pool;
    component->pool = pool_candidate;
    pool_candidate = NULL;
    component->buffer_width = (unsigned int)width;
    component->buffer_height = (unsigned int)height;
    component->layout = layout;
    component->presentation_generation = generation;
    component->rendered_composition_revision =
        current_composition_revision(component);
    component->presentation_pending = true;
    boxrig_wayland_input_cancel_scrollbar(component, NULL);
    component->scrollbar_visibility.state.reserved_width = bar_width;
    component->scrollbar_visibility.target_width = bar_width;
    component->scale_numerator = numerator;
    component->buffer_scale = buffer_scale;
    component->rendering_policy = policy;
    boxrig_font_visual_step_sequence_destroy(component->visual_steps);
    component->visual_steps = NULL;
    component->visual_step_policy_valid = false;
    if (component->scale_revision != UINT64_MAX) {
        component->scale_revision++;
    }
    component->scale_dirty = false;
    if (wl_proxy_get_version((struct wl_proxy *)component->surface) >=
        WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_set_buffer_scale(component->surface, buffer_scale);
    }
    if (component->viewport != NULL) {
        wp_viewport_set_destination(
            component->viewport,
            (int32_t)component->logical_width,
            (int32_t)component->logical_height);
    }
    component->context->flush_required = true;
    boxrig_wayland_shm_pool_destroy(previous_pool);
    boxrig_wayland_input_update_cursor_rectangle(component);
    return BOXRIG_UI_OK;

failure:
    (void)component_renderer_set_scrollbar(component, component->scrollbar_visibility.state.visible,
        scrollbar_pixel_width(component->scale_numerator));
    boxrig_pango_cairo_renderer_font_candidate_destroy(font_candidate);
    if (boxrig_pango_cairo_renderer_surface_candidate_active(
            component->renderer)) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(
            component->renderer);
    }
    boxrig_wayland_shm_pool_destroy(pool_candidate);
    return status;
}

static void retain_view_dirty(
    boxrig_wayland_terminal_component *component)
{
    boxrig_font_effective_metrics metrics;
    boxrig_ui_dirty dirty;
    size_t row;
    bool retained = false;

    if (boxrig_ui_view_peek_dirty(
            component->view,
            BOXRIG_UI_DIRTY_PRESENT,
            &dirty)) {
        boxrig_wayland_shm_pool_add_full_dirty(component->pool);
        return;
    }
    if (boxrig_pango_cairo_renderer_get_effective_metrics(
            component->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        metrics.cell_height == 0) {
        boxrig_wayland_shm_pool_add_full_dirty(component->pool);
        return;
    }
    for (row = 0; row < component->layout.rows; row++) {
        boxrig_ui_dirty_layer layer;
        bool row_dirty = false;

        for (layer = BOXRIG_UI_DIRTY_MODEL;
             layer <= BOXRIG_UI_DIRTY_OVERLAY;
             layer++) {
            if (boxrig_ui_view_get_row_dirty(
                    component->view,
                    layer,
                    row,
                    &dirty)) {
                row_dirty = true;
                break;
            }
        }
        if (row_dirty) {
            uint64_t top = (uint64_t)row * metrics.cell_height;
            uint64_t bottom = top + metrics.cell_height;

            if (top >= component->buffer_height) {
                continue;
            }
            if (bottom > component->buffer_height) {
                bottom = component->buffer_height;
            }
            boxrig_wayland_shm_pool_add_dirty(
                component->pool,
                0,
                (int32_t)top,
                (int32_t)component->buffer_width,
                (int32_t)(bottom - top));
            retained = true;
        }
    }
    if (!retained) {
        boxrig_wayland_shm_pool_add_full_dirty(component->pool);
    }
}

static void dirty_surface_for_buffer(
    boxrig_wayland_terminal_component *component,
    const boxrig_wayland_shm_buffer *buffer)
{
    bool full_dirty = buffer->full_dirty || !buffer->presented ||
                       buffer->dirty_count == 0;
    size_t index;

    if (wl_proxy_get_version((struct wl_proxy *)component->surface) >=
        WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        if (full_dirty) {
            wl_surface_damage_buffer(
                component->surface,
                0,
                0,
                (int32_t)component->buffer_width,
                (int32_t)component->buffer_height);
            return;
        }
        for (index = 0; index < buffer->dirty_count; index++) {
            const boxrig_wayland_dirty_rectangle *rectangle =
                &buffer->dirty_rectangles[index];

            wl_surface_damage_buffer(
                component->surface,
                rectangle->x,
                rectangle->y,
                rectangle->width,
                rectangle->height);
        }
        return;
    }
    if (full_dirty) {
        wl_surface_damage(
            component->surface,
            0,
            0,
            (int32_t)component->logical_width,
            (int32_t)component->logical_height);
        return;
    }
    for (index = 0; index < buffer->dirty_count; index++) {
        const boxrig_wayland_dirty_rectangle *rectangle =
            &buffer->dirty_rectangles[index];
        uint64_t left = (uint64_t)rectangle->x * 120u /
                        component->scale_numerator;
        uint64_t top = (uint64_t)rectangle->y * 120u /
                       component->scale_numerator;
        uint64_t right =
            ((uint64_t)(rectangle->x + rectangle->width) * 120u +
             component->scale_numerator - 1u) /
            component->scale_numerator;
        uint64_t bottom =
            ((uint64_t)(rectangle->y + rectangle->height) * 120u +
             component->scale_numerator - 1u) /
            component->scale_numerator;

        if (right > component->logical_width) {
            right = component->logical_width;
        }
        if (bottom > component->logical_height) {
            bottom = component->logical_height;
        }
        wl_surface_damage(
            component->surface,
            (int32_t)left,
            (int32_t)top,
            (int32_t)(right - left),
            (int32_t)(bottom - top));
    }
}

static void set_animation_deadline(
    const boxrig_wayland_terminal_component *component,
    const boxrig_terminal *terminal,
    boxrig_ui_component_frame_result *result_out)
{
    result_out->has_animation_deadline = boxrig_terminal_next_animation_deadline(
        terminal, &result_out->animation_deadline_milliseconds);
    uint64_t blink_deadline;
    if (boxrig_ui_view_next_animation_deadline(component->view, &blink_deadline) &&
        (!result_out->has_animation_deadline ||
         blink_deadline < result_out->animation_deadline_milliseconds)) {
        result_out->has_animation_deadline = true;
        result_out->animation_deadline_milliseconds = blink_deadline;
    }
}

static boxrig_ui_status frame_opengl(
    boxrig_wayland_terminal_component *component,
    boxrig_terminal *terminal,
    boxrig_ui_component_frame_result *result_out)
{
    boxrig_ui_layout_action action;
    bool callback_created = false;
    bool presentation_allowed =
        !component->visible || component->frame_callback == NULL ||
        component->presentation_immediate;
    bool draw = component_frame_requested(component) ||
                component->presentation_pending;
    boxrig_ui_status status;

    if (draw && presentation_allowed) {
        if (component->visible) {
            status = apply_surface_regions(component);
            if (status != BOXRIG_UI_OK) {
                return status;
            }
            status = ensure_frame_callback(component, &callback_created);
            if (status != BOXRIG_UI_OK) {
                return status;
            }
        }
        if (component_renderer_draw_view_composition(
                component,
                component->visible) == -1) {
            if (callback_created) {
                wl_callback_destroy(component->frame_callback);
                component->frame_callback = NULL;
            }
            return BOXRIG_UI_UNAVAILABLE;
        }
        boxrig_wayland_input_update_cursor_rectangle(component);
        component->rendered_composition_revision =
            current_composition_revision(component);
        if (component->presentation_generation != UINT64_MAX) {
            component->presentation_generation++;
        }
        component->presentation_pending = false;
        component->presentation_immediate = false;
        component->context->flush_required = true;
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->frame_pending = component->presentation_pending ||
                                component_frame_requested(component);
    result_out->input_blocked = component->input_blocked;
    result_out->additional_dispatch =
        boxrig_ui_view_peek_layout_action(component->view, &action);
    set_animation_deadline(component, terminal, result_out);
    return BOXRIG_UI_OK;
}

static bool layout_transaction_waiting(
    const boxrig_wayland_terminal_component *component,
    bool *ready_out)
{
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state state;

    *ready_out = false;
    if (!component->layout_presentation_barrier) {
        return false;
    }
    if (component->dispatched_grid_resize.revision != 0) {
        return true;
    }
    if (boxrig_ui_view_peek_layout_action(component->view, &action)) {
        *ready_out = true;
        return true;
    }
    return boxrig_ui_view_get_size_command_state(component->view, &state) ==
               BOXRIG_UI_OK &&
           state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED;
}

static void set_layout_wait_frame_result(
    boxrig_wayland_terminal_component *component,
    boxrig_terminal *terminal,
    bool ready,
    boxrig_ui_component_frame_result *result_out)
{
    memset(result_out, 0, sizeof(*result_out));
    result_out->frame_pending = true;
    result_out->input_blocked = component->input_blocked;
    result_out->additional_dispatch = ready;
    set_animation_deadline(component, terminal, result_out);
}

boxrig_ui_status boxrig_wayland_terminal_component_frame(
    boxrig_wayland_terminal_component *component,
    boxrig_ui_component_frame_result *result_out)
{
    boxrig_terminal *terminal;
    cairo_surface_t *canonical;
    boxrig_wayland_shm_buffer *buffer = NULL;
    boxrig_renderer_frame_token frame_token;
    boxrig_ui_layout_action action;
    size_t buffer_index = 0;
    size_t index;
    boxrig_ui_status status;
    bool frame_prepared = false;
    bool presentation_allowed;
    bool draw_requested;
    bool layout_ready;

    if (component == NULL || result_out == NULL || !component->running ||
        !component->allocated) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    terminal = boxrig_ui_model_terminal(component->model);
    if (terminal == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_ui_view_animation_status(component->view);
    boxrig_composition_view composition;
    if (status != BOXRIG_UI_OK) return status;
    if (boxrig_composition_get_view(component->composition, &composition) !=
        BOXRIG_COMPOSITION_OK) return BOXRIG_UI_UNAVAILABLE;
    /* Active preedit precedes the renderer's visibility calculation. */
    status = boxrig_ui_view_set_composition_active(component->view,
        composition.active && composition.focused);
    if (status == BOXRIG_UI_OK) status = boxrig_ui_model_synchronize(component->model);
    if (status != BOXRIG_UI_OK) return status;
    status = boxrig_wayland_terminal_component_set_scrollbar_visibility(component,
        component->scrollbar_visibility.state.requested);
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_BUSY) return status;
    if (layout_transaction_waiting(component, &layout_ready)) {
        set_layout_wait_frame_result(
            component, terminal, layout_ready, result_out);
        return BOXRIG_UI_OK;
    }
    if (boxrig_terminal_synchronized_output(terminal)) {
        /* The renderer may skip a held frame. Keep its composition revision
         * pending instead of accepting an unchanged presentation. */
        memset(result_out, 0, sizeof(*result_out));
        result_out->frame_pending = component->presentation_pending ||
                                    component_frame_requested(component);
        result_out->input_blocked = component->input_blocked;
        result_out->additional_dispatch =
            boxrig_ui_view_peek_layout_action(component->view, &action);
        set_animation_deadline(component, terminal, result_out);
        return BOXRIG_UI_OK;
    }
    status = apply_pending_scale(component);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (layout_transaction_waiting(component, &layout_ready)) {
        set_layout_wait_frame_result(
            component, terminal, layout_ready, result_out);
        return BOXRIG_UI_OK;
    }
    if (component_uses_presentation_renderer(component)) {
        return frame_opengl(component, terminal, result_out);
    }
    presentation_allowed = !component->visible ||
                           component->frame_callback == NULL ||
                           component->presentation_immediate;
    draw_requested = component_frame_requested(component);
    if (draw_requested) {
        retain_view_dirty(component);
    }
    if (draw_requested && presentation_allowed) {
        for (index = 0; index < component->pool->buffer_count; index++) {
            if (!component->pool->buffers[index].busy) {
                buffer = &component->pool->buffers[index];
                buffer_index = index;
                break;
            }
        }
    }
    if (buffer != NULL && draw_requested) {
        status = ensure_renderer_lifecycle(component);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        if (boxrig_renderer_lifecycle_prepare_frame(
                component->renderer_lifecycle,
                component->view,
                component->composition,
                component->renderer_target.generation,
                &frame_token) != BOXRIG_RENDERER_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        frame_prepared = true;
        boxrig_wayland_input_update_cursor_rectangle(component);
        if (component->presentation_generation != UINT64_MAX) {
            component->presentation_generation++;
        }
        component->presentation_pending = true;
        canonical = boxrig_pango_cairo_renderer_surface(
            component->renderer);
        status = boxrig_wayland_shm_buffer_copy_cairo(
            component->pool,
            buffer_index,
            canonical,
            component->presentation_generation);
        if (status != BOXRIG_UI_OK) {
            (void)boxrig_renderer_lifecycle_abort_frame(
                component->renderer_lifecycle,
                &frame_token);
            return status;
        }
        if (!component->visible) {
            if (boxrig_renderer_lifecycle_accept_frame(
                    component->renderer_lifecycle,
                    &frame_token) != BOXRIG_RENDERER_OK) {
                return BOXRIG_UI_UNAVAILABLE;
            }
            component->rendered_composition_revision =
                current_composition_revision(component);
            frame_prepared = false;
        }
    }
    if (component->visible && component->presentation_pending &&
        (component->frame_callback == NULL ||
         component->presentation_immediate)) {
        if (buffer == NULL) {
            for (index = 0; index < component->pool->buffer_count; index++) {
                if (!component->pool->buffers[index].busy) {
                    buffer = &component->pool->buffers[index];
                    buffer_index = index;
                    break;
                }
            }
        }
        if (buffer != NULL) {
            canonical = boxrig_pango_cairo_renderer_surface(
                component->renderer);
            if (buffer->generation != component->presentation_generation) {
                status = boxrig_wayland_shm_buffer_copy_cairo(
                    component->pool,
                    buffer_index,
                    canonical,
                    component->presentation_generation);
                if (status != BOXRIG_UI_OK) {
                    if (frame_prepared) {
                        (void)boxrig_renderer_lifecycle_abort_frame(
                            component->renderer_lifecycle,
                            &frame_token);
                    }
                    return status;
                }
            }
            wl_surface_attach(component->surface, buffer->proxy, 0, 0);
            dirty_surface_for_buffer(component, buffer);
            status = apply_surface_regions(component);
            if (status != BOXRIG_UI_OK) {
                if (frame_prepared) {
                    (void)boxrig_renderer_lifecycle_abort_frame(
                        component->renderer_lifecycle,
                        &frame_token);
                }
                return status;
            }
            if (component->frame_callback == NULL) {
                component->frame_callback = wl_surface_frame(
                    component->surface);
                if (component->frame_callback == NULL) {
                    if (frame_prepared) {
                        (void)boxrig_renderer_lifecycle_abort_frame(
                            component->renderer_lifecycle,
                            &frame_token);
                    }
                    return BOXRIG_UI_OUT_OF_MEMORY;
                }
                wl_proxy_set_queue(
                    (struct wl_proxy *)component->frame_callback,
                    component->context->queue);
                if (wl_callback_add_listener(
                        component->frame_callback,
                        &frame_listener,
                        component) != 0) {
                    wl_callback_destroy(component->frame_callback);
                    component->frame_callback = NULL;
                    if (frame_prepared) {
                        (void)boxrig_renderer_lifecycle_abort_frame(
                            component->renderer_lifecycle,
                            &frame_token);
                    }
                    return BOXRIG_UI_UNAVAILABLE;
                }
            }
            buffer->busy = true;
            wl_surface_commit(component->surface);
            if (frame_prepared &&
                boxrig_renderer_lifecycle_accept_frame(
                    component->renderer_lifecycle,
                    &frame_token) != BOXRIG_RENDERER_OK) {
                return BOXRIG_UI_UNAVAILABLE;
            }
            if (frame_prepared) {
                component->rendered_composition_revision =
                    current_composition_revision(component);
            }
            frame_prepared = false;
            boxrig_wayland_shm_buffer_clear_dirty(buffer);
            component->context->flush_required = true;
            component->presentation_pending = false;
            component->presentation_immediate = false;
        }
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->frame_pending = component->presentation_pending ||
                                component_frame_requested(component);
    result_out->input_blocked = component->input_blocked;
    result_out->additional_dispatch =
        boxrig_ui_view_peek_layout_action(component->view, &action);
    set_animation_deadline(component, terminal, result_out);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_wayland_terminal_component_hit_test(
    boxrig_wayland_terminal_component *component,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out)
{
    int64_t numerator;

    if (component == NULL || result_out == NULL || !component->allocated ||
        component->scale_numerator == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    numerator = (int64_t)component->scale_numerator;
    if (x > INT64_MAX / numerator || x < INT64_MIN / numerator ||
        y > INT64_MAX / numerator || y < INT64_MIN / numerator) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    return boxrig_ui_view_hit_test_with_composition(
        component->view,
        component->composition,
        (x * numerator) / 120,
        (y * numerator) / 120,
        result_out);
}

static boxrig_ui_status set_transparency_opengl(
    boxrig_wayland_terminal_component *component,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_state previous;
    boxrig_ui_transparency_state candidate;
    uint64_t generation;
    bool callback_created = false;
    bool renderer_changed = false;
    bool surface_candidate = false;
    bool view_changed = false;
    boxrig_ui_status status;

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
    if (!component->allocated ||
        (candidate.effective == previous.effective &&
         candidate.effective_background_opacity ==
             previous.effective_background_opacity)) {
        if (component_renderer_apply_transparency_state(
                component,
                &candidate) == -1) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        status = boxrig_ui_view_set_transparency_config(
            component->view,
            config);
        if (status != BOXRIG_UI_OK) {
            (void)component_renderer_apply_transparency_state(
                component,
                &previous);
        }
        return status;
    }
    if (component_renderer_prepare_surface_candidate(
            component,
            component->buffer_width,
            component->buffer_height) == -1) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    surface_candidate = true;
    if (component_renderer_apply_transparency_state(
            component,
            &candidate) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    renderer_changed = true;
    status = boxrig_ui_view_set_transparency_config(component->view, config);
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    view_changed = true;
    if (component_renderer_draw_view_composition(component, false) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    if (component->visible) {
        status = apply_surface_regions(component);
        if (status != BOXRIG_UI_OK) {
            goto failure;
        }
        status = ensure_frame_callback(component, &callback_created);
        if (status != BOXRIG_UI_OK) {
            goto failure;
        }
    }
    if (component_renderer_commit_surface_candidate(
            component,
            component->visible) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    surface_candidate = false;
    generation = component->presentation_generation == UINT64_MAX
                     ? UINT64_MAX
                     : component->presentation_generation + 1;
    component->presentation_generation = generation;
    component->rendered_composition_revision =
        current_composition_revision(component);
    component->presentation_pending = !component->visible;
    component->presentation_immediate = !component->visible;
    component->context->flush_required = true;
    boxrig_wayland_input_update_cursor_rectangle(component);
    return BOXRIG_UI_OK;

failure:
    if (callback_created) {
        wl_callback_destroy(component->frame_callback);
        component->frame_callback = NULL;
    }
    if (view_changed) {
        (void)boxrig_ui_view_set_transparency_config(
            component->view,
            &previous.config);
    }
    if (renderer_changed) {
        (void)component_renderer_apply_transparency_state(
            component,
            &previous);
    }
    if (surface_candidate) {
        component_renderer_abort_surface_candidate(component);
    }
    return status;
}

boxrig_ui_status boxrig_wayland_terminal_component_set_transparency(
    boxrig_wayland_terminal_component *component,
    const boxrig_ui_transparency_config *config)
{
    boxrig_wayland_shm_pool *pool_candidate = NULL;
    boxrig_wayland_shm_pool *previous_pool;
    boxrig_ui_transparency_state previous;
    boxrig_ui_transparency_state candidate;
    cairo_surface_t *surface;
    uint64_t generation;
    uint32_t format;
    bool renderer_changed = false;
    bool view_changed = false;
    boxrig_ui_status status;

    if (component == NULL || config == NULL || !component->running) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component_uses_presentation_renderer(component)) {
        return set_transparency_opengl(component, config);
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
    if (!component->allocated ||
        (candidate.effective == previous.effective &&
         candidate.effective_background_opacity ==
             previous.effective_background_opacity)) {
        if (boxrig_pango_cairo_renderer_apply_transparency_state(
                component->renderer,
                &candidate) == -1) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        status = boxrig_ui_view_set_transparency_config(
            component->view,
            config);
        if (status != BOXRIG_UI_OK) {
            (void)boxrig_pango_cairo_renderer_apply_transparency_state(
                component->renderer,
                &previous);
        }
        return status;
    }
    format = candidate.effective ==
                     BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA
                 ? WL_SHM_FORMAT_ARGB8888
                 : WL_SHM_FORMAT_XRGB8888;
    if ((format == WL_SHM_FORMAT_ARGB8888 && !component->context->argb8888) ||
        (format == WL_SHM_FORMAT_XRGB8888 && !component->context->xrgb8888)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_wayland_shm_pool_create(
        component->context,
        component->config.buffer_count,
        component->buffer_width,
        component->buffer_height,
        format,
        component->config.maximum_bytes,
        component->config.maximum_dirty_rectangles,
        &pool_candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (boxrig_pango_cairo_renderer_prepare_surface_candidate(
            component->renderer,
            component->buffer_width,
            component->buffer_height) == -1) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto failure;
    }
    if (boxrig_pango_cairo_renderer_apply_transparency_state(
            component->renderer,
            &candidate) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    renderer_changed = true;
    status = boxrig_ui_view_set_transparency_config(component->view, config);
    if (status != BOXRIG_UI_OK) {
        goto failure;
    }
    view_changed = true;
    if (boxrig_pango_cairo_renderer_draw_view_composition(
            component->renderer,
            component->view,
            component->composition) == -1) {
        status = BOXRIG_UI_UNAVAILABLE;
        goto failure;
    }
    generation = component->presentation_generation == UINT64_MAX
                     ? UINT64_MAX
                     : component->presentation_generation + 1;
    surface = boxrig_pango_cairo_renderer_surface(component->renderer);
    status = boxrig_wayland_shm_pool_copy_cairo(
        pool_candidate,
        surface,
        generation);
    if (status != BOXRIG_UI_OK ||
        boxrig_pango_cairo_renderer_commit_surface_candidate(
            component->renderer) == -1) {
        status = status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
        goto failure;
    }
    previous_pool = component->pool;
    component->pool = pool_candidate;
    component->presentation_generation = generation;
    component->rendered_composition_revision =
        current_composition_revision(component);
    component->presentation_pending = true;
    /* A format and alpha-policy transition is surface state, not an
     * animation frame. Commit it even while an earlier frame hint is
     * outstanding so an idle surface cannot defer the transition. */
    component->presentation_immediate = true;
    boxrig_wayland_shm_pool_destroy(previous_pool);
    boxrig_wayland_input_update_cursor_rectangle(component);
    return BOXRIG_UI_OK;

failure:
    if (view_changed) {
        (void)boxrig_ui_view_set_transparency_config(
            component->view,
            &previous.config);
    }
    if (renderer_changed) {
        (void)boxrig_pango_cairo_renderer_apply_transparency_state(
            component->renderer,
            &previous);
    }
    if (boxrig_pango_cairo_renderer_surface_candidate_active(
            component->renderer)) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(
            component->renderer);
    }
    boxrig_wayland_shm_pool_destroy(pool_candidate);
    return status;
}

boxrig_ui_status boxrig_wayland_terminal_component_get_transparency(
    const boxrig_wayland_terminal_component *component,
    boxrig_ui_transparency_state *state_out)
{
    if (component == NULL || state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_transparency(component->view, state_out);
}

bool boxrig_wayland_terminal_component_take_layout_action(
    boxrig_wayland_terminal_component *component,
    boxrig_ui_layout_action *action_out)
{
    boxrig_ui_size_command_state state;
    boxrig_ui_layout_action action;

    if (component == NULL || action_out == NULL || !component->running ||
        component->dispatched_grid_resize.revision != 0 ||
        boxrig_ui_view_get_size_command_state(component->view, &state) !=
            BOXRIG_UI_OK ||
        state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED ||
        !boxrig_ui_view_take_layout_action(component->view, &action)) {
        return false;
    }
    if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        component->dispatched_grid_resize.revision = action.revision;
        component->dispatched_grid_resize.rows =
            action.value.grid_resize.rows;
        component->dispatched_grid_resize.columns =
            action.value.grid_resize.columns;
    }
    *action_out = action;
    return true;
}

boxrig_ui_status boxrig_wayland_terminal_component_complete_size_command(
    boxrig_wayland_terminal_component *component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    boxrig_ui_layout_action action;
    boxrig_ui_status status;

    if (component == NULL || !component->running ||
        component->dispatched_grid_resize.revision != 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_complete_size_command(
        component->view,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
    if (status == BOXRIG_UI_OK && applied_out != NULL && *applied_out) {
        component->layout_presentation_barrier =
            boxrig_ui_view_peek_layout_action(component->view, &action);
    }
    return status;
}

boxrig_ui_status boxrig_wayland_terminal_component_complete_grid_resize(
    boxrig_wayland_terminal_component *component,
    uint64_t revision,
    bool succeeded)
{
    boxrig_terminal *terminal;
    size_t rows;
    size_t columns;

    if (component == NULL || revision == 0 || !component->running) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (component->dispatched_grid_resize.revision != revision) {
        return BOXRIG_UI_STALE;
    }
    if (!succeeded) {
        return boxrig_wayland_terminal_component_quiesce(component);
    }
    terminal = boxrig_ui_model_terminal(component->model);
    if (terminal == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows != component->dispatched_grid_resize.rows ||
        columns != component->dispatched_grid_resize.columns) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(
        &component->dispatched_grid_resize,
        0,
        sizeof(component->dispatched_grid_resize));
    {
        boxrig_ui_layout_action action;

        component->layout_presentation_barrier =
            boxrig_ui_view_peek_layout_action(component->view, &action);
    }
    return commit_scrollbar_visibility(component);
}

bool boxrig_wayland_terminal_component_take_host_action(
    boxrig_wayland_terminal_component *component,
    boxrig_wayland_host_action *action_out)
{
    if (component == NULL || action_out == NULL || !component->running ||
        component->parent_state_revision ==
            component->effective_parent_state_revision ||
        component->parent_action_taken) {
        return false;
    }
    action_out->revision = component->parent_state_revision;
    action_out->parent_commit_required = true;
    action_out->x = component->candidate_x;
    action_out->y = component->candidate_y;
    component->parent_action_taken = true;
    return true;
}

boxrig_ui_status boxrig_wayland_terminal_component_complete_parent_commit(
    boxrig_wayland_terminal_component *component,
    uint64_t revision,
    bool *applied_out)
{
    if (component == NULL || applied_out == NULL || revision == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *applied_out = false;
    if (!component->running || !component->parent_action_taken ||
        revision != component->parent_state_revision) {
        return BOXRIG_UI_OK;
    }
    component->effective_parent_state_revision = revision;
    component->effective_x = component->candidate_x;
    component->effective_y = component->candidate_y;
    component->parent_action_taken = false;
    if (component->allocation_sync_pending) {
        wl_subsurface_set_desync(component->subsurface);
        component->allocation_sync_pending = false;
        component->context->flush_required = true;
    }
    *applied_out = true;
    return BOXRIG_UI_OK;
}

uint32_t boxrig_wayland_terminal_component_capabilities(
    const boxrig_wayland_terminal_component *component)
{
    uint32_t capabilities;

    if (component == NULL) {
        return 0;
    }
    capabilities = BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD |
                   BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY |
                   BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES |
                   BOXRIG_UI_COMPONENT_CAPABILITY_SIZE_COMMAND;
    if (component_uses_presentation_renderer(component)
            ? component->presentation_alpha_target
            : component->context->argb8888) {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA;
    }
    if (component->context->text_input_manager != NULL) {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_INPUT_METHOD;
    }
    if (component->context->data_device_manager != NULL ||
        component->context->primary_selection_manager != NULL) {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_CLIPBOARD_PASTE;
    }
    return capabilities;
}

boxrig_ui_status boxrig_wayland_terminal_component_get_diagnostic(
    const boxrig_wayland_terminal_component *component,
    boxrig_wayland_component_diagnostic *diagnostic_out)
{
    if (component == NULL || diagnostic_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    diagnostic_out->running = component->running;
    diagnostic_out->parent_commit_pending =
        component->parent_state_revision !=
        component->effective_parent_state_revision;
    diagnostic_out->parent_state_revision =
        component->parent_state_revision;
    diagnostic_out->effective_parent_state_revision =
        component->effective_parent_state_revision;
    diagnostic_out->effective_x = component->effective_x;
    diagnostic_out->effective_y = component->effective_y;
    diagnostic_out->buffer_count = component->config.buffer_count;
    diagnostic_out->logical_width = component->logical_width;
    diagnostic_out->logical_height = component->logical_height;
    diagnostic_out->buffer_width = component->buffer_width;
    diagnostic_out->buffer_height = component->buffer_height;
    diagnostic_out->memory_bytes = component->pool == NULL
                                       ? 0
                                       : component->pool->mapping_size +
                                             component->pool->buffer_size;
    diagnostic_out->presentation_generation =
        component->presentation_generation;
    if (component->pool != NULL) {
        size_t index;

        for (index = 0; index < component->pool->buffer_count; index++) {
            if (component->pool->buffers[index].busy) {
                diagnostic_out->busy_buffer_count++;
            }
        }
    }
    diagnostic_out->frame_callback_pending =
        component->frame_callback != NULL;
    diagnostic_out->presentation_pending =
        component->presentation_pending;
    if (component->pool != NULL) {
        diagnostic_out->dirty_rectangle_high_water =
            component->pool->dirty_rectangle_high_water;
        diagnostic_out->dirty_collapse_count =
            component->pool->dirty_collapse_count;
    }
    diagnostic_out->scale_numerator = component->scale_numerator;
    diagnostic_out->buffer_scale = component->buffer_scale;
    diagnostic_out->fractional_scale_active =
        component->fractional_scale != NULL && component->viewport != NULL;
    diagnostic_out->entered_output_count = component->entered_output_count;
    diagnostic_out->scale_revision = component->scale_revision;
    if (diagnostic_out->fractional_scale_active && component->allocated) {
        diagnostic_out->viewport_width = component->logical_width;
        diagnostic_out->viewport_height = component->logical_height;
    }
    diagnostic_out->shm_format =
        component->pool == NULL ? 0 : component->pool->format;
    diagnostic_out->failure_reason = component->failure_reason !=
                                             BOXRIG_WAYLAND_FAILURE_NONE
                                         ? component->failure_reason
                                         : component->context->failure_reason;
    if (diagnostic_out->running &&
        diagnostic_out->failure_reason == BOXRIG_WAYLAND_FAILURE_NONE &&
        boxrig_ui_view_animation_status(component->view) != BOXRIG_UI_OK) {
        diagnostic_out->running = false;
        diagnostic_out->failure_reason = BOXRIG_WAYLAND_FAILURE_ANIMATION;
    }
    diagnostic_out->keyboard_focus_count = component->keyboard_focus_count;
    diagnostic_out->pointer_focus_count = component->pointer_focus_count;
    diagnostic_out->input_queue_depth = component->input_queue_depth;
    diagnostic_out->input_queue_high_water =
        component->input_queue_high_water;
    diagnostic_out->input_blocked = component->input_blocked;
    diagnostic_out->ime_waiting = component->ime_waiting;
    boxrig_ime_diagnostic ibus;
    boxrig_ime_get_diagnostic(component->context->ibus, &ibus);
    bool pending = component->context->state == BOXRIG_WAYLAND_CONTEXT_INITIALIZING ||
        ibus.state == BOXRIG_IME_CONNECTING;
    boxrig_ui_candidate_owner owner = boxrig_ime_candidate_owner(component->context->ibus, &component->ibus_target);
    diagnostic_out->candidate_policy = component->context->config.candidate_policy;
    diagnostic_out->candidate_owner = pending ? BOXRIG_UI_CANDIDATE_OWNER_NONE :
        owner;
    diagnostic_out->candidate_reason = pending ? BOXRIG_UI_CANDIDATE_PENDING :
        diagnostic_out->candidate_policy == BOXRIG_UI_CANDIDATE_SYSTEM ? BOXRIG_UI_CANDIDATE_SYSTEM_REQUEST :
        ibus.state == BOXRIG_IME_UNAVAILABLE ? BOXRIG_UI_CANDIDATE_IME_UNAVAILABLE :
        owner == BOXRIG_UI_CANDIDATE_OWNER_NONE ? BOXRIG_UI_CANDIDATE_UNCONFIRMED :
        owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT ? BOXRIG_UI_CANDIDATE_READY : BOXRIG_UI_CANDIDATE_STANDARD_INPUT;
    boxrig_ui_candidate_view candidates;
    (void)boxrig_ui_view_get_candidates(component->view, &candidates);
    diagnostic_out->candidate_revision = candidates.revision;
    diagnostic_out->candidate_count = candidates.snapshot.count;
    diagnostic_out->candidate_cursor = candidates.snapshot.cursor;
    diagnostic_out->candidate_visible = candidates.snapshot.visible;
    diagnostic_out->auxiliary_visible = candidates.snapshot.auxiliary_visible || candidates.snapshot.auxiliary_above_visible;
    if (boxrig_wayland_ibus_selected(component->context) && ibus.backend != BOXRIG_IME_BACKEND_NONE)
        diagnostic_out->input_capabilities |= ibus.backend == BOXRIG_IME_BACKEND_FCITX5 ?
            BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX5 : ibus.backend == BOXRIG_IME_BACKEND_FCITX4 ?
            BOXRIG_WAYLAND_INPUT_CAPABILITY_FCITX4 : BOXRIG_WAYLAND_INPUT_CAPABILITY_IBUS;
    diagnostic_out->pressed_key_count = component->pressed_key_count;
    diagnostic_out->pressed_button_count = component->pressed_button_count;
    diagnostic_out->input_error_count = component->input_error_count;
    boxrig_wayland_clipboard_get_component_diagnostic(
        component,
        diagnostic_out);
    if (component->context->compose_table != NULL) {
        diagnostic_out->input_capabilities |=
            BOXRIG_WAYLAND_INPUT_CAPABILITY_XKB_COMPOSE;
    }
    if (component->context->text_input_manager != NULL) {
        diagnostic_out->input_capabilities |=
            BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3;
    }
    {
        const boxrig_wayland_seat *seat;

        for (seat = component->context->seats;
             seat != NULL;
             seat = seat->next) {
            if (seat->keymap_available) {
                diagnostic_out->input_capabilities |=
                    BOXRIG_WAYLAND_INPUT_CAPABILITY_KEYBOARD;
            }
            if (seat->pointer != NULL) {
                diagnostic_out->input_capabilities |=
                    BOXRIG_WAYLAND_INPUT_CAPABILITY_POINTER;
            }
            if (seat->text_enabled && seat->text_component == component) {
                diagnostic_out->text_input_focus_count++;
            }
        }
    }
    if (component->composition != NULL) {
        boxrig_composition_view composition;

        if (boxrig_composition_get_view(
                component->composition,
                &composition) == BOXRIG_COMPOSITION_OK) {
            diagnostic_out->ime_active = composition.active;
            diagnostic_out->ime_commit_pending = composition.commit_pending;
        }
    }
    return BOXRIG_UI_OK;
}
