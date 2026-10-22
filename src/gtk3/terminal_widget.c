/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/gtk3/terminal_widget.h"
#include "../ui/scrollbar_visibility.h"

#include "boxrig/ui/lifecycle.h"
#include "input.h"
#include "accessible.h"
#include "../gtk/animation.h"
#include "../ui/view_private.h"
#include "../pangocairo/renderer.h"
#include "../pangocairo/renderer_private.h"

#ifndef BOXRIG_GTK_OPENGL
#define BOXRIG_GTK_OPENGL 0
#endif
#if BOXRIG_GTK_OPENGL
#include "../gtk/gl_area.h"
#include "../gtk/gl_surface.h"
#endif

#include <cairo/cairo.h>
#include <gtk/gtk-a11y.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOXRIG_GTK3_DEFAULT_PASTE_CAPACITY (8U * 1024U * 1024U)
#define BOXRIG_GTK3_MAXIMUM_PASTE_CAPACITY (256U * 1024U * 1024U)
#define BOXRIG_GTK3_DEFAULT_PASTE_CHUNK_SIZE 4096U
#define BOXRIG_GTK3_MAXIMUM_PASTE_CHUNK_SIZE (64U * 1024U)
#define BOXRIG_GTK3_DEFAULT_CLIPBOARD_TRANSFERS 8U
#define BOXRIG_GTK3_MAXIMUM_CLIPBOARD_TRANSFERS 64U
#define BOXRIG_GTK3_DEFAULT_CLIPBOARD_BYTES (64U * 1024U * 1024U)
#define BOXRIG_GTK3_MAXIMUM_CLIPBOARD_BYTES (256U * 1024U * 1024U)

enum {
    PROP_0,
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY
};

typedef struct boxrig_gtk3_font_specification_storage {
    boxrig_font_specification specification;
    boxrig_font_family *families;
    boxrig_font_feature *features;
    boxrig_font_variation *variations;
} boxrig_gtk3_font_specification_storage;

struct _BoxrigGtk3TerminalWidget {
    GtkContainer parent_instance;
    boxrig_ui_model *model;
    bool accessibility_enabled;
    boxrig_ui_view *view;
    boxrig_pango_cairo_renderer *renderer;
    boxrig_gtk_render_state render_state;
    GtkWidget *gl_area;
    bool gl_failed;
    bool in_gl_render;
    bool cairo_failed;
    bool gl_pending;
    bool gl_presented;
#if BOXRIG_GTK_OPENGL
    bool gl_desktop_only;
    bool gl_retry_desktop;
    boxrig_gtk_gl_surface *gl_surface;
    uint64_t gl_font_revision;
    boxrig_text_direction_policy gl_direction_policy;
#endif
    boxrig_gtk3_input *input;
    GdkWindow *event_window;
    cairo_surface_t *presentation;
    cairo_surface_t *presentation_candidate;
    GtkScrolledWindow *scroll_container;
    boxrig_ui_scrollbar_visibility_controller scrollbar_visibility;
    unsigned int scrollbar_scale;
    unsigned int scrollbar_target_output_width;
    bool scrollbar_native_pending;
    GtkAdjustment *horizontal_adjustment;
    GtkAdjustment *vertical_adjustment;
    gulong vertical_adjustment_handler;
    GMainContext *main_context;
    boxrig_gtk_animation *animations;
    boxrig_ui_status animation_failure;
    GThread *owner_thread;
    boxrig_ui_lifecycle lifecycle;
    boxrig_gtk3_font_specification_storage font;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_visual_step_sequence *visual_steps;
    boxrig_font_visual_step_policy visual_step_policy;
    boxrig_font_effective_metrics active_zoom_metrics;
    boxrig_font_effective_metrics pending_zoom_metrics;
    boxrig_text_direction_policy direction_policy;
    boxrig_ui_window_state window_state;
    GtkScrollablePolicy horizontal_policy;
    GtkScrollablePolicy vertical_policy;
    boxrig_ui_frame_request_callback frame_request_callback;
    void *frame_request_context;
    unsigned int output_width;
    unsigned int output_height;
    unsigned int output_scale;
    unsigned int surface_candidate_output_width;
    unsigned int surface_candidate_output_height;
    uint64_t presentation_generation;
    uint64_t presentation_composition_revision;
    uint64_t surface_candidate_composition_revision;
    uint64_t allocation_generation;
    uint64_t allocation_metrics_revision;
    uint64_t presentation_allocation_generation;
    uint64_t drawn_allocation_generation;
    boxrig_ui_grid_resize pending_grid_resize;
    boxrig_ui_grid_resize dispatched_grid_resize;
    boxrig_ui_fixed surface_candidate_width;
    boxrig_ui_fixed surface_candidate_height;
    uint64_t surface_candidate_revision;
    size_t maximum_native_input_events;
    size_t maximum_frame_items;
    bool allocated;
    bool allocation_pending;
    bool presentation_visible;
    bool updating_adjustments;
    bool in_frame;
    bool visual_step_policy_valid;
    bool zoom_generation_active;
    bool zoom_target_pending;
    bool surface_candidate_ready;
    bool surface_candidate_committed;
    bool accessibility_busy;
    bool disposed;
};

static void boxrig_gtk3_terminal_widget_scrollable_init(
    GtkScrollableInterface *interface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    BoxrigGtk3TerminalWidget,
    boxrig_gtk3_terminal_widget,
    GTK_TYPE_CONTAINER,
    G_IMPLEMENT_INTERFACE(
        GTK_TYPE_SCROLLABLE,
        boxrig_gtk3_terminal_widget_scrollable_init))

static bool valid_attachment(boxrig_ui_component_attachment attachment)
{
    return attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY ||
           attachment == BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
}

static bool uses_gl(const BoxrigGtk3TerminalWidget *widget)
{
    return (widget->cairo_failed && widget->gl_presented) ||
        (widget->render_state.requested_policy != BOXRIG_GTK_RENDER_CAIRO && !widget->gl_failed);
}

static bool has_presentation(const BoxrigGtk3TerminalWidget *widget)
{
    return widget->gl_presented || widget->presentation != NULL;
}

static void font_specification_storage_destroy(
    boxrig_gtk3_font_specification_storage *storage)
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
    boxrig_gtk3_font_specification_storage *storage_out)
{
    boxrig_gtk3_font_specification_storage storage;
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

static bool valid_window_state(boxrig_ui_window_state state)
{
    return state >= BOXRIG_UI_WINDOW_NORMAL &&
           state <= BOXRIG_UI_WINDOW_EMBEDDED;
}

static bool accepts_calls(const BoxrigGtk3TerminalWidget *widget)
{
    return widget != NULL && !widget->disposed &&
           widget->owner_thread == g_thread_self() &&
           boxrig_ui_lifecycle_accepts_events(&widget->lifecycle);
}

static void set_accessibility_busy(
    BoxrigGtk3TerminalWidget *widget,
    bool busy)
{
    if (widget == NULL || widget->accessibility_busy == busy) {
        return;
    }
    widget->accessibility_busy = busy;
    boxrig_gtk3_accessible_set_busy(widget, busy);
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
        return BOXRIG_UI_INVALID_ARGUMENT;
    case BOXRIG_FONT_CATALOG_IO_ERROR:
    case BOXRIG_FONT_CATALOG_INVALID_FONT:
    case BOXRIG_FONT_CATALOG_NOT_FOUND:
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_UNAVAILABLE;
}

static GtkAdjustment *create_horizontal_adjustment(void)
{
    return GTK_ADJUSTMENT(gtk_adjustment_new(0.0, 0.0, 1.0, 1.0, 1.0, 1.0));
}

static GtkAdjustment *create_vertical_adjustment(void)
{
    return GTK_ADJUSTMENT(gtk_adjustment_new(0.0, 0.0, 1.0, 1.0, 1.0, 1.0));
}

static void request_application_frame(BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_frame_request_callback callback = widget->frame_request_callback;
    void *context = widget->frame_request_context;

    if (callback != NULL && !widget->disposed) {
        callback(context);
    }
}

static void retain_latest_grid_resize(BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_layout_action action;

    if (boxrig_ui_view_peek_layout_action(widget->view, &action) &&
        action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE &&
        boxrig_ui_view_take_layout_action(widget->view, &action)) {
        widget->pending_grid_resize.revision = action.revision;
        widget->pending_grid_resize.rows = action.value.grid_resize.rows;
        widget->pending_grid_resize.columns =
            action.value.grid_resize.columns;
    }
}

static void input_activity(void *context)
{
    BoxrigGtk3TerminalWidget *widget = context;

    /* Pending dirty may be held by synchronized output without a scheduled
     * frame. Native input still needs its own dispatch wakeup in that state. */
    request_application_frame(widget);
}

static void vertical_adjustment_changed(
    GtkAdjustment *adjustment,
    gpointer user_data)
{
    BoxrigGtk3TerminalWidget *widget = user_data;
    double value;
    size_t position;
    bool changed = false;

    if (widget->updating_adjustments || !accepts_calls(widget) ||
        widget->model == NULL) {
        return;
    }
    value = gtk_adjustment_get_value(adjustment);
    if (value <= 0.0) {
        position = 0;
    } else if (value >= (double)SIZE_MAX) {
        position = SIZE_MAX;
    } else {
        position = (size_t)(value + 0.5);
    }
    if (boxrig_scrollbar_apply_action(
            boxrig_ui_model_terminal(widget->model),
            BOXRIG_SCROLLBAR_POSITION,
            position,
            &changed) == 0 && changed) {
        request_application_frame(widget);
    }
}

static void replace_horizontal_adjustment(
    BoxrigGtk3TerminalWidget *widget,
    GtkAdjustment *adjustment)
{
    GtkAdjustment *replacement = adjustment;

    if (replacement == NULL) {
        replacement = create_horizontal_adjustment();
        g_set_object(&widget->horizontal_adjustment, replacement);
        g_object_unref(replacement);
    } else {
        g_set_object(&widget->horizontal_adjustment, replacement);
    }
}

static void replace_vertical_adjustment(
    BoxrigGtk3TerminalWidget *widget,
    GtkAdjustment *adjustment)
{
    GtkAdjustment *replacement = adjustment;

    if (widget->vertical_adjustment != NULL &&
        widget->vertical_adjustment_handler != 0) {
        g_signal_handler_disconnect(
            widget->vertical_adjustment,
            widget->vertical_adjustment_handler);
        widget->vertical_adjustment_handler = 0;
    }
    if (replacement == NULL) {
        replacement = create_vertical_adjustment();
        g_set_object(&widget->vertical_adjustment, replacement);
        g_object_unref(replacement);
    } else {
        g_set_object(&widget->vertical_adjustment, replacement);
    }
    widget->vertical_adjustment_handler = g_signal_connect(
        widget->vertical_adjustment,
        "value-changed",
        G_CALLBACK(vertical_adjustment_changed),
        widget);
}

static void configure_adjustment_if_changed(
    GtkAdjustment *adjustment,
    double value,
    double lower,
    double upper,
    double step_increment,
    double page_increment,
    double page_size)
{
    /* GTK 3 configure emits changed even when every value is unchanged. A
     * scrollbar responds by requesting layout, which synchronizes us again.
     * Read the live object because GtkScrollable exposes it to the parent. */
    if (gtk_adjustment_get_value(adjustment) == value &&
        gtk_adjustment_get_lower(adjustment) == lower &&
        gtk_adjustment_get_upper(adjustment) == upper &&
        gtk_adjustment_get_step_increment(adjustment) == step_increment &&
        gtk_adjustment_get_page_increment(adjustment) == page_increment &&
        gtk_adjustment_get_page_size(adjustment) == page_size) {
        return;
    }
    gtk_adjustment_configure(
        adjustment, value, lower, upper, step_increment, page_increment, page_size);
}

static void synchronize_adjustments(BoxrigGtk3TerminalWidget *widget)
{
    boxrig_scrollbar_model model;
    double upper;
    double page;

    if (widget->horizontal_adjustment == NULL ||
        widget->vertical_adjustment == NULL || widget->model == NULL ||
        boxrig_scrollbar_model_read(
            boxrig_ui_model_terminal(widget->model),
            &model) == -1) {
        return;
    }
    page = (double)model.viewport_rows;
    upper = model.enabled ? (double)model.history_rows + page : page;
    if (page < 1.0) {
        page = 1.0;
    }
    if (upper < page) {
        upper = page;
    }
    widget->updating_adjustments = true;
    configure_adjustment_if_changed(
        widget->horizontal_adjustment,
        0.0,
        0.0,
        1.0,
        1.0,
        1.0,
        1.0);
    configure_adjustment_if_changed(
        widget->vertical_adjustment,
        model.enabled ? (double)model.position : 0.0,
        0.0,
        upper,
        1.0,
        page,
        page);
    widget->updating_adjustments = false;
}

static void invalidate_complete_presentation(
    BoxrigGtk3TerminalWidget *widget)
{
    boxrig_terminal *terminal;
    boxrig_ui_dirty dirty;

    if (widget->view == NULL || widget->model == NULL) {
        return;
    }
    terminal = boxrig_ui_model_terminal(widget->model);
    if (terminal == NULL) {
        return;
    }
    boxrig_terminal_size(
        terminal,
        &dirty.row_count,
        &dirty.column_count);
    dirty.row = 0;
    dirty.column = 0;
    if (dirty.row_count != 0 && dirty.column_count != 0) {
        (void)boxrig_ui_view_invalidate(
            widget->view,
            BOXRIG_UI_DIRTY_PRESENT,
            dirty);
    }
}

static bool has_scroll_container(const BoxrigGtk3TerminalWidget *widget)
{
    return widget->scroll_container != NULL &&
        gtk_widget_get_parent(GTK_WIDGET(widget)) == GTK_WIDGET(widget->scroll_container);
}

static unsigned int standard_scrollbar_width(const BoxrigGtk3TerminalWidget *widget)
{
    GtkWidget *bar = gtk_scrolled_window_get_vscrollbar(widget->scroll_container);
    int minimum = 0;
    gtk_widget_get_preferred_width(bar, &minimum, NULL);
    return minimum > 0 ? (unsigned int)minimum : 1;
}

static void apply_scrollbar_native_policy(BoxrigGtk3TerminalWidget *widget)
{
    if (!widget->scrollbar_visibility.state.pending || !has_scroll_container(widget)) return;
    widget->scrollbar_native_pending = true;
    /* Resolve AUTO before asking GTK to allocate, so reflow cannot turn a
     * native automatic-policy decision into another resize transaction. */
    gtk_scrolled_window_set_policy(widget->scroll_container, GTK_POLICY_NEVER,
        widget->scrollbar_visibility.target_visible ? GTK_POLICY_ALWAYS : GTK_POLICY_NEVER);
    gtk_widget_queue_resize(GTK_WIDGET(widget->scroll_container));
    if (!widget->allocated) {
        boxrig_ui_scrollbar_visibility_commit(&widget->scrollbar_visibility);
        widget->scrollbar_native_pending = false;
    }
    request_application_frame(widget);
}

static void finish_scrollbar_allocation(BoxrigGtk3TerminalWidget *widget)
{
    if (!has_scroll_container(widget)) return;
    if (widget->scrollbar_visibility.state.pending && !widget->scrollbar_native_pending) return;
    const int outer = gtk_widget_get_allocated_width(GTK_WIDGET(widget->scroll_container));
    const int inner = gtk_widget_get_allocated_width(GTK_WIDGET(widget));
    const unsigned int width = outer > inner ? (unsigned int)(outer - inner) : 0;
    const unsigned int scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    widget->scrollbar_visibility.target_width = widget->scrollbar_visibility.target_visible ? width * scale : 0;
    widget->scrollbar_scale = scale;
    boxrig_ui_scrollbar_visibility_commit(&widget->scrollbar_visibility);
    widget->scrollbar_native_pending = false;
}

GtkScrolledWindow *boxrig_gtk3_terminal_widget_create_scroll_container(BoxrigGtk3TerminalWidget *widget)
{
    if (!accepts_calls(widget) || gtk_widget_get_parent(GTK_WIDGET(widget)) != NULL) return NULL;
    if (widget->scroll_container != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(widget->scroll_container), (gpointer *)&widget->scroll_container);
        widget->scroll_container = NULL;
    }
    GtkScrolledWindow *container = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_scrolled_window_set_policy(container, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_overlay_scrolling(container, FALSE);
    gtk_scrolled_window_set_propagate_natural_width(container, TRUE);
    gtk_scrolled_window_set_propagate_natural_height(container, TRUE);
    gtk_scrolled_window_set_shadow_type(container, GTK_SHADOW_NONE);
    gtk_scrolled_window_set_placement(container, GTK_CORNER_TOP_LEFT);
    gtk_container_add(GTK_CONTAINER(container), GTK_WIDGET(widget));
    widget->scroll_container = container;
    g_object_add_weak_pointer(G_OBJECT(container), (gpointer *)&widget->scroll_container);
    boxrig_ui_scrollbar_visibility policy = widget->scrollbar_visibility.state.requested;
    boxrig_scrollbar_state scrollbar;
    bool enabled = boxrig_ui_view_get_scrollbar_state(widget->view, &scrollbar) == BOXRIG_UI_OK &&
        scrollbar.model.enabled;
    bool visible = policy == BOXRIG_UI_SCROLLBAR_ALWAYS ||
        (policy == BOXRIG_UI_SCROLLBAR_AUTO && enabled);
    widget->scrollbar_scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    boxrig_ui_scrollbar_visibility_initialize(&widget->scrollbar_visibility, visible,
        standard_scrollbar_width(widget) * widget->scrollbar_scale);
    widget->scrollbar_visibility.state.requested = policy;
    /* The new container has no presented allocation yet. Its first native
     * allocation owns grid negotiation, including when reparenting a widget. */
    gtk_scrolled_window_set_policy(container, GTK_POLICY_NEVER,
        visible ? GTK_POLICY_ALWAYS : GTK_POLICY_NEVER);
    return container;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_scrollbar_visibility(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_scrollbar_visibility policy)
{
    if (!accepts_calls(widget) || policy < BOXRIG_UI_SCROLLBAR_ALWAYS || policy > BOXRIG_UI_SCROLLBAR_NEVER)
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (!has_scroll_container(widget)) {
        boxrig_ui_scrollbar_visibility_initialize(&widget->scrollbar_visibility, false, 0);
        widget->scrollbar_visibility.state.requested = policy;
        widget->scrollbar_native_pending = false;
        return BOXRIG_UI_OK;
    }
    if (widget->scrollbar_visibility.state.pending || widget->pending_grid_resize.revision != 0 ||
        widget->dispatched_grid_resize.revision != 0 || widget->zoom_generation_active) return BOXRIG_UI_BUSY;
    boxrig_ui_status status = boxrig_ui_model_synchronize(widget->model);
    if (status != BOXRIG_UI_OK) return status;
    unsigned int scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    unsigned int width = standard_scrollbar_width(widget) * scale;
    boxrig_ui_allocation_input input = {0};
    if (widget->allocated) {
        boxrig_font_effective_metrics metrics;
        if (boxrig_pango_cairo_renderer_get_effective_metrics(widget->renderer, &metrics) != BOXRIG_FONT_CATALOG_OK)
            return BOXRIG_UI_UNAVAILABLE;
        unsigned int old_width = widget->scrollbar_visibility.state.reserved_width;
        input.allocation_width = ((boxrig_ui_fixed)widget->output_width + old_width) * BOXRIG_UI_FIXED_ONE;
        input.allocation_height = (boxrig_ui_fixed)widget->output_height * BOXRIG_UI_FIXED_ONE;
        input.cell_width = (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
        input.cell_height = (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
        input.cell_metrics_revision = metrics.layout_revision;
        input.window_state = widget->window_state;
    }
    status = boxrig_ui_scrollbar_visibility_prepare(&widget->scrollbar_visibility, widget->view,
        policy, true, width, widget->allocated ? &input : NULL);
    if (status != BOXRIG_UI_OK || !widget->scrollbar_visibility.state.pending) return status;
    boxrig_ui_fixed reserved = (boxrig_ui_fixed)widget->scrollbar_visibility.target_width * BOXRIG_UI_FIXED_ONE;
    widget->scrollbar_target_output_width = input.allocation_width > reserved
        ? (unsigned int)((input.allocation_width - reserved) / BOXRIG_UI_FIXED_ONE) : 1;
    widget->scrollbar_scale = scale;
    retain_latest_grid_resize(widget);
    if (widget->pending_grid_resize.revision == 0) apply_scrollbar_native_policy(widget);
    invalidate_complete_presentation(widget);
    request_application_frame(widget);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_scrollbar_visibility(
    const BoxrigGtk3TerminalWidget *widget, boxrig_ui_scrollbar_visibility_state *state_out)
{
    if (widget == NULL || state_out == NULL || widget->owner_thread != g_thread_self())
        return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = widget->scrollbar_visibility.state;
    if (!has_scroll_container(widget)) {
        state_out->visible = false;
        state_out->reserved_width = 0;
        state_out->pending = false;
    } else if (widget->scrollbar_scale != 0) {
        state_out->reserved_width /= widget->scrollbar_scale;
    }
    return BOXRIG_UI_OK;
}

static uint64_t current_composition_revision(const BoxrigGtk3TerminalWidget *widget)
{
    boxrig_composition_view view;
    return boxrig_composition_get_view(
        boxrig_gtk3_input_composition(widget->input), &view) == BOXRIG_COMPOSITION_OK
        ? view.revision : 0;
}

#if BOXRIG_GTK_OPENGL
static bool input_geometry_ready(const BoxrigGtk3TerminalWidget *widget);

static void gl_failed(BoxrigGtk3TerminalWidget *widget)
{
    GdkGLContext *context = gtk_gl_area_get_context(GTK_GL_AREA(widget->gl_area));
    if (widget->gl_surface == NULL && !widget->gl_desktop_only &&
        widget->render_state.status == BOXRIG_UI_UNAVAILABLE &&
        context != NULL && gdk_gl_context_get_use_es(context))
        widget->gl_retry_desktop = true;
    widget->gl_failed = true;
    widget->gl_pending = false;
    widget->gl_presented = false;
    if (widget->render_state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL)
        widget->render_state.active_backend = BOXRIG_GTK_RENDER_BACKEND_NONE;
    widget->render_state.pending = widget->render_state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE &&
        (widget->gl_retry_desktop ||
         widget->render_state.requested_policy == BOXRIG_GTK_RENDER_OPENGL_PREFERRED);
    invalidate_complete_presentation(widget);
    request_application_frame(widget);
}

static void gl_realize(GtkGLArea *area, gpointer data)
{
    BoxrigGtk3TerminalWidget *widget = data;
    gtk_gl_area_make_current(area);
    const GError *error = gtk_gl_area_get_error(area);
    if (error != NULL) {
        boxrig_gtk_gl_area_record_error(&widget->render_state,
            BOXRIG_GTK_RENDER_REASON_CONTEXT, BOXRIG_UI_UNAVAILABLE, error->message);
        gl_failed(widget);
        return;
    }
    widget->render_state.context_generation++;
    widget->render_state.pending = true;
    widget->gl_pending = true;
    request_application_frame(widget);
}

static void gl_unrealize(GtkGLArea *area, gpointer data)
{
    BoxrigGtk3TerminalWidget *widget = data;
    gtk_gl_area_make_current(area);
    boxrig_gtk_gl_surface_destroy(&widget->gl_surface);
    widget->gl_presented = false;
    widget->gl_pending = false;
    if (widget->render_state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL) {
        widget->render_state.active_backend = BOXRIG_GTK_RENDER_BACKEND_NONE;
        widget->render_state.pending = true;
        invalidate_complete_presentation(widget);
    }
}

static void gl_mapped(GtkWidget *area, gpointer data)
{
    (void)area;
    BoxrigGtk3TerminalWidget *widget = data;
    /* GLArea maps its own input-only sibling window. Keep the terminal's
     * input owner above it, including remaps and runtime renderer changes. */
    if (widget->event_window != NULL) gdk_window_raise(widget->event_window);
}

static gboolean render_gl_frame(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    BoxrigGtk3TerminalWidget *widget = data;
    (void)context;
    if (!accepts_calls(widget) || !uses_gl(widget) || !widget->allocated) return TRUE;
    /* The host may change the terminal after frame() queues GTK work. Match
     * the view's dimensions and dirty to that model before preparing GL. */
    boxrig_ui_status model_status = boxrig_ui_model_synchronize(widget->model);
    if (model_status != BOXRIG_UI_OK) {
        boxrig_gtk_gl_area_record_error(&widget->render_state,
            BOXRIG_GTK_RENDER_REASON_DRAW, model_status, "Could not synchronize the terminal before GTK rendering");
        gl_failed(widget);
        return TRUE;
    }
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    uint64_t generation = widget->render_state.target_generation;
    if (generation == 0) generation = 1;
    if (boxrig_gtk_gl_area_describe(area, widget->render_state.context_generation,
            generation, &config, &widget->render_state) != BOXRIG_UI_OK) {
        gl_failed(widget);
        return TRUE;
    }
    boxrig_gl_target destination = config.initial_target;
    boxrig_font_effective_metrics metrics;
    if (boxrig_pango_cairo_renderer_get_effective_metrics(widget->renderer, &metrics) != BOXRIG_FONT_CATALOG_OK) {
        boxrig_gtk_gl_area_record_error(&widget->render_state,
            BOXRIG_GTK_RENDER_REASON_RESOURCE, BOXRIG_UI_UNAVAILABLE, "Could not resolve GTK cell metrics");
        gl_failed(widget);
        return TRUE;
    }
    boxrig_gl_target previous = {0};
    (void)boxrig_gtk_gl_surface_get_target(widget->gl_surface, &previous);
    bool replace = widget->gl_surface == NULL || previous.format != destination.format ||
        previous.alpha_mode != destination.alpha_mode || previous.origin != destination.origin ||
        widget->gl_font_revision != metrics.layout_revision ||
        widget->gl_direction_policy != widget->direction_policy;
    bool resize = !replace && (previous.width != destination.width ||
        previous.height != destination.height);
    boxrig_gtk_gl_surface *candidate = NULL;
    if (replace) {
        boxrig_pango_cairo_renderer_font_selection font;
        if (boxrig_pango_cairo_renderer_resolve_font_selection(widget->renderer,
                &widget->font.specification, &font) != BOXRIG_FONT_CATALOG_OK) {
            boxrig_gtk_gl_area_record_error(&widget->render_state,
                BOXRIG_GTK_RENDER_REASON_RESOURCE, BOXRIG_UI_UNAVAILABLE, "Could not resolve GTK font resources");
            gl_failed(widget);
            return TRUE;
        }
        config.font_family = font.family_list;
        config.font_family_length = strlen(font.family_list);
        config.font_files = font.resource_paths;
        config.font_file_count = font.resource_path_count;
        config.font_size_dip = metrics.requested_size_dip;
        config.font_weight = widget->font.specification.weight;
        config.font_style = widget->font.specification.style;
        config.font_stretch = widget->font.specification.stretch;
        config.font_fallback = widget->font.specification.fallback;
        config.font_features = widget->font.specification.features;
        config.font_feature_count = widget->font.specification.feature_count;
        config.font_variations = widget->font.specification.variations;
        config.font_variation_count = widget->font.specification.variation_count;
        config.effective_metrics = &metrics;
        config.rendering_policy = widget->rendering_policy;
        config.direction_policy = widget->direction_policy;
        config.initial_target.generation = generation + 1;
        destination.generation = config.initial_target.generation;
        config.runtime.target_generation = generation + 1;
        boxrig_ui_status status = boxrig_gtk_gl_surface_create(&config, &candidate, &widget->render_state);
        boxrig_pango_cairo_renderer_font_selection_destroy(&font);
        if (status != BOXRIG_UI_OK) {
            gl_failed(widget);
            return TRUE;
        }
        invalidate_complete_presentation(widget);
    } else if (resize) {
        destination.generation = generation + 1;
        if (boxrig_gtk_gl_surface_resize(widget->gl_surface, &destination,
                &widget->render_state) != BOXRIG_UI_OK) {
            boxrig_gtk_gl_surface_destroy(&widget->gl_surface);
            gl_failed(widget);
            return TRUE;
        }
        invalidate_complete_presentation(widget);
    }
    boxrig_gtk_gl_surface *surface = replace ? candidate : widget->gl_surface;
    bool changed = replace || resize || widget->gl_pending || boxrig_ui_view_frame_requested(widget->view);
    if ((changed && boxrig_gtk_gl_surface_draw(surface, widget->view,
            boxrig_gtk3_input_composition(widget->input), &widget->render_state) != BOXRIG_UI_OK) ||
        boxrig_gtk_gl_surface_present(surface, &destination, &widget->render_state) != BOXRIG_UI_OK) {
        boxrig_gtk_gl_surface_destroy(&candidate);
        if (resize) boxrig_gtk_gl_surface_destroy(&widget->gl_surface);
        gl_failed(widget);
        return TRUE;
    }
    if (replace) {
        boxrig_gtk_gl_surface_destroy(&widget->gl_surface);
        widget->gl_surface = candidate;
        widget->gl_font_revision = metrics.layout_revision;
        widget->gl_direction_policy = widget->direction_policy;
        widget->render_state.target_generation = config.initial_target.generation;
    } else if (resize) {
        widget->render_state.target_generation = destination.generation;
    }
    widget->gl_pending = false;
    widget->gl_presented = true;
    widget->render_state.pending = false;
    bool notify = widget->render_state.active_backend != BOXRIG_GTK_RENDER_BACKEND_OPENGL ||
        widget->zoom_generation_active;
    widget->render_state.active_backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
    widget->presentation_composition_revision = current_composition_revision(widget);
    widget->presentation_allocation_generation = widget->allocation_generation;
    if (changed) {
        if (widget->presentation_generation != UINT64_MAX) widget->presentation_generation++;
        if (input_geometry_ready(widget))
            boxrig_gtk3_input_update_candidate(widget->input, widget->output_scale);
        if (notify) request_application_frame(widget);
    }
    /* GTK rendering can consume a newer model and row mapping than the
     * frame() that queued it. Publish that presented viewport as well. */
    if (changed && accepts_calls(widget) &&
        !boxrig_terminal_synchronized_output(boxrig_ui_model_terminal(widget->model)) &&
        boxrig_gtk3_accessible_update(widget) != BOXRIG_UI_OK)
        request_application_frame(widget);
    return TRUE;
}
static gboolean gl_render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    BoxrigGtk3TerminalWidget *widget = data;
    if (widget->in_frame || widget->in_gl_render) return TRUE;
    g_object_ref(widget);
    widget->in_gl_render = true;
    gboolean result = render_gl_frame(area, context, data);
    widget->in_gl_render = false;
    g_object_unref(widget);
    return result;
}

static void create_gl_area(BoxrigGtk3TerminalWidget *widget)
{
    widget->gl_area = boxrig_gtk_gl_area_new();
    if (widget->gl_desktop_only)
        gtk_gl_area_set_use_es(GTK_GL_AREA(widget->gl_area), FALSE);
    g_signal_connect(widget->gl_area, "realize", G_CALLBACK(gl_realize), widget);
    g_signal_connect(widget->gl_area, "unrealize", G_CALLBACK(gl_unrealize), widget);
    g_signal_connect(widget->gl_area, "map", G_CALLBACK(gl_mapped), widget);
    g_signal_connect(widget->gl_area, "render", G_CALLBACK(gl_render), widget);
    gtk_widget_set_parent(widget->gl_area, GTK_WIDGET(widget));
    gtk_widget_show(widget->gl_area);
    gtk_widget_queue_allocate(GTK_WIDGET(widget));
}

static bool retain_previous_gl(BoxrigGtk3TerminalWidget *widget, boxrig_ui_status status, const char *message)
{
    if (!widget->render_state.pending || !widget->gl_presented) return false;
    boxrig_gtk_gl_area_record_error(&widget->render_state, BOXRIG_GTK_RENDER_REASON_RESOURCE, status, message);
    widget->render_state.pending = false;
    widget->cairo_failed = true;
    widget->gl_pending = true;
    invalidate_complete_presentation(widget);
    gtk_gl_area_queue_render(GTK_GL_AREA(widget->gl_area));
    return true;
}

static void release_gl_area(BoxrigGtk3TerminalWidget *widget)
{
    if (widget->gl_area == NULL) return;
    gtk_widget_destroy(widget->gl_area);
}
#endif

static void terminal_forall(GtkContainer *container, gboolean include_internals,
    GtkCallback callback, gpointer data)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(container);
    if (include_internals && widget->gl_area != NULL) callback(widget->gl_area, data);
}

static void terminal_remove(GtkContainer *container, GtkWidget *child)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(container);
    if (child == widget->gl_area) {
        widget->gl_area = NULL;
        gtk_widget_unparent(child);
    }
}

static void clear_surface_candidate_state(
    BoxrigGtk3TerminalWidget *widget)
{
    g_clear_pointer(
        &widget->presentation_candidate,
        cairo_surface_destroy);
    widget->surface_candidate_width = 0;
    widget->surface_candidate_height = 0;
    widget->surface_candidate_output_width = 0;
    widget->surface_candidate_output_height = 0;
    widget->surface_candidate_revision = 0;
    widget->surface_candidate_composition_revision = 0;
    widget->surface_candidate_ready = false;
    widget->surface_candidate_committed = false;
}

static void abort_surface_candidate(BoxrigGtk3TerminalWidget *widget)
{
    if (widget->renderer != NULL &&
        boxrig_pango_cairo_renderer_surface_candidate_active(
            widget->renderer)) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(
            widget->renderer);
    }
    clear_surface_candidate_state(widget);
}

static boxrig_ui_status consume_presentation_dirty(boxrig_ui_view *view)
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

static boxrig_ui_status apply_layout_change_for_metrics(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_effective_metrics *metrics,
    boxrig_ui_layout_change_kind kind)
{
    boxrig_ui_layout_change_input input;
    boxrig_ui_layout_result layout;

    if (!widget->allocated) {
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
        widget->view,
        &input,
        &layout);
}

static boxrig_ui_status prepare_widget_font(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_ui_layout_change_kind change_kind,
    bool update_raw_state)
{
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    boxrig_gtk3_font_specification_storage storage;
    boxrig_font_effective_metrics metrics;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (!accepts_calls(widget) || specification == NULL || policy == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&storage, 0, sizeof(storage));
    font_status = boxrig_pango_cairo_renderer_prepare_font(
        widget->renderer,
        specification,
        policy,
        &candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return status_from_font_catalog(font_status);
    }
    font_status = boxrig_pango_cairo_renderer_font_candidate_metrics(
        candidate,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return status_from_font_catalog(font_status);
    }
    status = boxrig_ui_view_validate_cell_metrics(widget->view, &metrics);
    if (status != BOXRIG_UI_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return status;
    }
    if (update_raw_state &&
        !font_specification_storage_copy(specification, &storage)) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (update_raw_state && widget->surface_candidate_ready) {
        abort_surface_candidate(widget);
    }
    if (boxrig_ui_view_is_layout_authority(widget->view)) {
        status = boxrig_ui_view_commit_cell_metrics(widget->view, &metrics);
        if (status != BOXRIG_UI_OK) {
            font_specification_storage_destroy(&storage);
            boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
            return status;
        }
    }
    font_status = boxrig_pango_cairo_renderer_commit_font(
        widget->renderer,
        candidate);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        font_specification_storage_destroy(&storage);
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
        return status_from_font_catalog(font_status);
    }
    if (update_raw_state) {
        font_specification_storage_destroy(&widget->font);
        widget->font = storage;
        widget->rendering_policy = *policy;
        boxrig_font_visual_step_sequence_destroy(widget->visual_steps);
        widget->visual_steps = NULL;
        widget->visual_step_policy_valid = false;
        widget->zoom_generation_active = false;
        widget->zoom_target_pending = false;
    }
    status = apply_layout_change_for_metrics(widget, &metrics, change_kind);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    gtk_widget_queue_resize(GTK_WIDGET(widget));
    invalidate_complete_presentation(widget);
    return BOXRIG_UI_OK;
}

static boxrig_ui_status set_widget_font(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_ui_layout_change_kind change_kind,
    bool update_raw_state)
{
    if (widget != NULL && (widget->in_frame || widget->scrollbar_visibility.state.pending)) {
        return BOXRIG_UI_BUSY;
    }
    return prepare_widget_font(widget, specification, policy, change_kind,
                               update_raw_state);
}

static boxrig_ui_status synchronize_output_scale(
    BoxrigGtk3TerminalWidget *widget)
{
    boxrig_font_rendering_policy policy;
    int scale;

    if (widget == NULL || widget->renderer == NULL || widget->view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    if (scale <= 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (widget->rendering_policy.output_scale == (float)scale) {
        return BOXRIG_UI_OK;
    }
    /* Keep the metrics used by the pending scrollbar resize until the host
     * acknowledges it. The resulting GTK allocation applies the latest scale. */
    if (widget->scrollbar_visibility.state.pending && !widget->scrollbar_native_pending)
        return BOXRIG_UI_OK;
    policy = widget->rendering_policy;
    policy.output_scale = (float)scale;
    /* Native allocation recovery can run inside frame. Public font mutations
     * still pass through set_widget_font and reject reentrant calls. */
    return prepare_widget_font(
        widget,
        &widget->font.specification,
        &policy,
        BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS,
        true);
}

static bool visual_step_policy_matches(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_visual_step_policy policy)
{
    return widget->visual_step_policy_valid &&
           widget->visual_step_policy.minimum_size_dip ==
               policy.minimum_size_dip &&
           widget->visual_step_policy.maximum_size_dip ==
               policy.maximum_size_dip &&
           widget->visual_step_policy.candidate_increment_dip ==
               policy.candidate_increment_dip;
}

static boxrig_font_catalog_status resolve_widget_zoom_metrics(
    float requested_size_dip,
    void *context,
    boxrig_font_effective_metrics *metrics_out)
{
    BoxrigGtk3TerminalWidget *widget = context;
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    boxrig_font_specification specification;
    boxrig_font_catalog_status status;

    if (widget == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    specification = widget->font.specification;
    specification.size_dip = requested_size_dip;
    specification.line_height_adjustment_dip = 0.0f;
    status = boxrig_pango_cairo_renderer_prepare_font(
        widget->renderer,
        &specification,
        &widget->rendering_policy,
        &candidate);
    if (status == BOXRIG_FONT_CATALOG_OK) {
        status = boxrig_pango_cairo_renderer_font_candidate_metrics(
            candidate,
            metrics_out);
    }
    boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
    return status;
}

static boxrig_ui_status commit_widget_zoom(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_font_specification specification = widget->font.specification;

    specification.size_dip = metrics->requested_size_dip;
    return set_widget_font(
        widget,
        &specification,
        &widget->rendering_policy,
        BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS,
        false);
}

static boxrig_ui_status queue_or_commit_widget_zoom(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_ui_status status;

    if (widget->zoom_generation_active) {
        if (metrics->requested_size_dip ==
            widget->active_zoom_metrics.requested_size_dip) {
            widget->zoom_target_pending = false;
        } else {
            widget->pending_zoom_metrics = *metrics;
            widget->zoom_target_pending = true;
        }
        return BOXRIG_UI_OK;
    }
    status = commit_widget_zoom(widget, metrics);
    if (status == BOXRIG_UI_OK) {
        widget->active_zoom_metrics = *metrics;
        widget->zoom_generation_active = true;
    }
    return status;
}

static boxrig_ui_status complete_widget_zoom_generation(
    BoxrigGtk3TerminalWidget *widget)
{
    boxrig_font_effective_metrics target;
    boxrig_ui_status status;

    if (!widget->zoom_generation_active) {
        return BOXRIG_UI_OK;
    }
    widget->zoom_generation_active = false;
    if (!widget->zoom_target_pending) {
        return BOXRIG_UI_OK;
    }
    target = widget->pending_zoom_metrics;
    widget->zoom_target_pending = false;
    status = commit_widget_zoom(widget, &target);
    if (status != BOXRIG_UI_OK) {
        widget->pending_zoom_metrics = target;
        widget->zoom_target_pending = true;
        return status;
    }
    widget->active_zoom_metrics = target;
    widget->zoom_generation_active = true;
    return BOXRIG_UI_OK;
}

static void scale_factor_changed(
    GObject *object,
    GParamSpec *specification,
    gpointer user_data)
{
    BoxrigGtk3TerminalWidget *widget = user_data;

    (void)object;
    (void)specification;
    if (synchronize_output_scale(widget) == BOXRIG_UI_OK) {
        gtk_widget_queue_resize(GTK_WIDGET(widget));
    }
}

static cairo_surface_t *create_presentation_surface(
    BoxrigGtk3TerminalWidget *widget,
    unsigned int output_width,
    unsigned int output_height)
{
    cairo_surface_t *source;
    cairo_surface_t *copy;
    cairo_t *copy_cairo;

    source = boxrig_pango_cairo_renderer_surface(widget->renderer);
    if (source == NULL || output_width > INT_MAX ||
        output_height > INT_MAX) {
        return NULL;
    }
    copy = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        (int)output_width,
        (int)output_height);
    if (copy == NULL ||
        cairo_surface_status(copy) != CAIRO_STATUS_SUCCESS) {
        if (copy != NULL) {
            cairo_surface_destroy(copy);
        }
        return NULL;
    }
    copy_cairo = cairo_create(copy);
    cairo_surface_flush(source);
    cairo_set_operator(copy_cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(copy_cairo, source, 0.0, 0.0);
    cairo_paint(copy_cairo);
    if (cairo_status(copy_cairo) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(copy_cairo);
        cairo_surface_destroy(copy);
        return NULL;
    }
    cairo_destroy(copy_cairo);
    cairo_surface_mark_dirty(copy);
    return copy;
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
    const BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_size_command_state state;

    if (widget->scrollbar_visibility.state.pending) return true;
    return boxrig_ui_view_get_size_command_state(
               widget->view,
               &state) == BOXRIG_UI_OK &&
           state != BOXRIG_UI_SIZE_COMMAND_IDLE;
}

static bool grid_resize_waiting(const BoxrigGtk3TerminalWidget *widget)
{
    return widget->pending_grid_resize.revision != 0 ||
           widget->dispatched_grid_resize.revision != 0;
}

static bool input_geometry_ready(const BoxrigGtk3TerminalWidget *widget)
{
    boxrig_font_effective_metrics metrics;

    return widget->allocated && !widget->allocation_pending &&
           !grid_resize_waiting(widget) &&
           !presentation_waits_for_allocation(widget) &&
           boxrig_pango_cairo_renderer_get_effective_metrics(
               widget->renderer, &metrics) == BOXRIG_FONT_CATALOG_OK &&
           metrics.layout_revision == widget->allocation_metrics_revision;
}

static bool grid_resize_ready(const BoxrigGtk3TerminalWidget *widget)
{
    boxrig_font_effective_metrics metrics;
    size_t rows;
    size_t columns;

    if (widget->pending_grid_resize.revision == 0 ||
        widget->dispatched_grid_resize.revision != 0 ||
        !boxrig_ui_view_is_layout_authority(widget->view) ||
        !widget->allocated || widget->allocation_pending ||
        !widget->presentation_visible ||
        !gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
        widget->drawn_allocation_generation != widget->allocation_generation ||
        (presentation_waits_for_allocation(widget) && !widget->scrollbar_visibility.state.pending) ||
        boxrig_pango_cairo_renderer_get_effective_metrics(
            widget->renderer, &metrics) != BOXRIG_FONT_CATALOG_OK ||
        metrics.layout_revision != widget->allocation_metrics_revision ||
        metrics.cell_width == 0 || metrics.cell_height == 0) {
        return false;
    }
    rows = widget->output_height / metrics.cell_height;
    columns = (widget->scrollbar_visibility.state.pending && !widget->scrollbar_native_pending
        ? widget->scrollbar_target_output_width : widget->output_width) / metrics.cell_width;
    /* A host size-command completion can precede the widget's allocation.
     * Only the grid fitted to the actual native extent may use its draw. */
    return widget->pending_grid_resize.rows == (rows == 0 ? 1 : rows) &&
           widget->pending_grid_resize.columns == (columns == 0 ? 1 : columns);
}

static bool surface_candidate_preparation_pending(
    const BoxrigGtk3TerminalWidget *widget)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(widget->model);
    boxrig_ui_layout_action action;

    return terminal != NULL &&
           !boxrig_terminal_synchronized_output(terminal) &&
           widget->zoom_generation_active &&
           !widget->surface_candidate_ready &&
           boxrig_ui_view_peek_layout_action(widget->view, &action) &&
           action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE &&
           action.value.content_size.cause ==
               BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
}

static boxrig_ui_status prepare_zoom_presentation_candidate(
    BoxrigGtk3TerminalWidget *widget,
    bool *prepared_out)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(widget->model);
    boxrig_ui_layout_action action;
    boxrig_ui_layout_result layout;
    cairo_surface_t *candidate;
    unsigned int width;
    unsigned int height;

    *prepared_out = false;
    if (terminal == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_terminal_synchronized_output(terminal) ||
        !widget->zoom_generation_active ||
        !boxrig_ui_view_peek_layout_action(widget->view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE ||
        action.value.content_size.cause !=
            BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS) {
        return BOXRIG_UI_OK;
    }
    if (boxrig_ui_view_get_layout(widget->view, &layout) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (widget->surface_candidate_ready) {
        return widget->surface_candidate_width == layout.grid_width &&
                       widget->surface_candidate_height == layout.grid_height
                   ? BOXRIG_UI_OK
                   : BOXRIG_UI_UNAVAILABLE;
    }
    if (!request_pixel_size(layout.grid_width, &width) ||
        !request_pixel_size(layout.grid_height, &height) ||
        widget->output_scale == 0 ||
        width > UINT_MAX - (widget->output_scale - 1) ||
        height > UINT_MAX - (widget->output_scale - 1)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    width = ((width + widget->output_scale - 1) /
             widget->output_scale) *
            widget->output_scale;
    height = ((height + widget->output_scale - 1) /
              widget->output_scale) *
             widget->output_scale;
    if (boxrig_pango_cairo_renderer_prepare_surface_candidate(
            widget->renderer,
            width,
            height) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_pango_cairo_renderer_draw_view_composition(
            widget->renderer,
            widget->view,
            boxrig_gtk3_input_composition(widget->input)) == -1 ||
        !boxrig_pango_cairo_renderer_surface_candidate_ready(
            widget->renderer)) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(
            widget->renderer);
        return BOXRIG_UI_UNAVAILABLE;
    }
    candidate = create_presentation_surface(
        widget,
        width,
        height);
    if (candidate == NULL) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(
            widget->renderer);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    widget->presentation_candidate = candidate;
    widget->surface_candidate_composition_revision = current_composition_revision(widget);
    widget->surface_candidate_width = layout.grid_width;
    widget->surface_candidate_height = layout.grid_height;
    widget->surface_candidate_output_width = width;
    widget->surface_candidate_output_height = height;
    widget->surface_candidate_revision = 0;
    widget->surface_candidate_ready = true;
    *prepared_out = true;
    return BOXRIG_UI_OK;
}

static void boxrig_gtk3_terminal_widget_get_property(
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *specification)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(object);

    switch (property_id) {
    case PROP_HADJUSTMENT:
        g_value_set_object(value, widget->horizontal_adjustment);
        break;
    case PROP_VADJUSTMENT:
        g_value_set_object(value, widget->vertical_adjustment);
        break;
    case PROP_HSCROLL_POLICY:
        g_value_set_enum(value, widget->horizontal_policy);
        break;
    case PROP_VSCROLL_POLICY:
        g_value_set_enum(value, widget->vertical_policy);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, specification);
        break;
    }
}

static void boxrig_gtk3_terminal_widget_set_property(
    GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *specification)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(object);

    /* GtkScrolledWindow clears the child's adjustments while the parent
     * dispose unparents it. Never recreate references after our own dispose. */
    if (widget->disposed) return;
    switch (property_id) {
    case PROP_HADJUSTMENT:
        replace_horizontal_adjustment(widget, g_value_get_object(value));
        break;
    case PROP_VADJUSTMENT:
        replace_vertical_adjustment(widget, g_value_get_object(value));
        synchronize_adjustments(widget);
        break;
    case PROP_HSCROLL_POLICY:
        widget->horizontal_policy = g_value_get_enum(value);
        gtk_widget_queue_resize(GTK_WIDGET(widget));
        break;
    case PROP_VSCROLL_POLICY:
        widget->vertical_policy = g_value_get_enum(value);
        gtk_widget_queue_resize(GTK_WIDGET(widget));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, specification);
        break;
    }
}

static void boxrig_gtk3_terminal_widget_preferred_size(
    GtkWidget *gtk_widget,
    bool horizontal,
    int *minimum,
    int *natural)
{
    BoxrigGtk3TerminalWidget *widget =
        BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);
    boxrig_font_effective_metrics metrics;
    size_t rows = 1;
    size_t columns = 1;
    uint64_t cell;
    uint64_t count;
    uint64_t minimum_value;
    uint64_t natural_value;
    unsigned int scale;

    scale = (unsigned int)gtk_widget_get_scale_factor(gtk_widget);
    if (scale == 0) {
        scale = 1;
    }
    memset(&metrics, 0, sizeof(metrics));
    if (widget->renderer == NULL ||
        boxrig_pango_cairo_renderer_get_effective_metrics(
            widget->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK) {
        metrics.cell_width = 1;
        metrics.cell_height = 1;
    }
    if (widget->view != NULL) {
        (void)boxrig_ui_view_get_preferred_grid(
            widget->view,
            &rows,
            &columns);
    }
    cell = horizontal ? metrics.cell_width : metrics.cell_height;
    count = horizontal ? columns : rows;
    minimum_value = (cell + scale - 1) / scale;
    natural_value = (cell * count + scale - 1) / scale;
    if (minimum_value == 0) {
        minimum_value = 1;
    }
    if (natural_value < minimum_value) {
        natural_value = minimum_value;
    }
    if (minimum != NULL) {
        *minimum = minimum_value > INT_MAX ? INT_MAX : (int)minimum_value;
    }
    if (natural != NULL) {
        *natural = natural_value > INT_MAX ? INT_MAX : (int)natural_value;
    }
}

static void boxrig_gtk3_terminal_widget_get_preferred_width(
    GtkWidget *gtk_widget,
    int *minimum,
    int *natural)
{
    boxrig_gtk3_terminal_widget_preferred_size(
        gtk_widget,
        true,
        minimum,
        natural);
}

static void boxrig_gtk3_terminal_widget_get_preferred_height(
    GtkWidget *gtk_widget,
    int *minimum,
    int *natural)
{
    boxrig_gtk3_terminal_widget_preferred_size(
        gtk_widget,
        false,
        minimum,
        natural);
}

static bool prepare_widget_allocation(
    BoxrigGtk3TerminalWidget *widget,
    const GtkAllocation *allocation)
{
    GtkWidget *gtk_widget = GTK_WIDGET(widget);
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_result layout;
    boxrig_ui_size_command_state command_state;
    int width;
    int height;
    unsigned int scale;
    unsigned int output_width;
    unsigned int output_height;
    bool preserve_candidate;
    bool resolve_candidate;
    bool commit_candidate;
    bool allocation_changed;
    bool presentation_covers_previous_allocation;
    bool surface_resized = false;
    bool allocation_was_pending = widget->allocation_pending;

    if (widget->scrollbar_visibility.state.pending && !widget->scrollbar_native_pending)
        return true;
    /* GTK has already changed its native allocation. Until preparation
     * succeeds, neither the old grid nor its old draw boundary is applicable. */
    widget->allocation_pending = true;
    width = allocation->width;
    height = allocation->height;
    if (!accepts_calls(widget) || width <= 0 || height <= 0) {
        return false;
    }
    scale = (unsigned int)gtk_widget_get_scale_factor(gtk_widget);
    if (scale == 0 || (unsigned int)width > UINT_MAX / scale ||
        (unsigned int)height > UINT_MAX / scale) {
        return false;
    }
    if (synchronize_output_scale(widget) != BOXRIG_UI_OK) {
        return false;
    }
    if (boxrig_pango_cairo_renderer_get_effective_metrics(
            widget->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK) {
        return false;
    }
    output_width = (unsigned int)width * scale;
    output_height = (unsigned int)height * scale;
    allocation_changed = allocation_was_pending || !widget->allocated ||
                         output_width != widget->output_width ||
                         output_height != widget->output_height ||
                         scale != widget->output_scale ||
                         metrics.layout_revision !=
                             widget->allocation_metrics_revision;
    preserve_candidate = widget->surface_candidate_ready &&
                         widget->allocated &&
                         output_width == widget->output_width &&
                         output_height == widget->output_height;
    resolve_candidate = widget->surface_candidate_ready &&
                        !preserve_candidate;
    commit_candidate = resolve_candidate &&
                       !widget->surface_candidate_committed &&
                       output_width ==
                           widget->surface_candidate_output_width &&
                       output_height ==
                           widget->surface_candidate_output_height;
    if (resolve_candidate && !commit_candidate) {
        abort_surface_candidate(widget);
    }
    if (!preserve_candidate && !commit_candidate &&
        (!widget->allocated || output_width != widget->output_width ||
         output_height != widget->output_height)) {
        if (boxrig_pango_cairo_renderer_set_surface_size(
                widget->renderer,
                output_width,
                output_height) == -1) {
            return false;
        }
        surface_resized = true;
    }
    memset(&input, 0, sizeof(input));
    input.allocation_width =
        (boxrig_ui_fixed)output_width * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)output_height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = widget->window_state;
    if (boxrig_ui_view_accept_allocation(
            widget->view,
            &input,
            &layout) != BOXRIG_UI_OK) {
        return false;
    }
    presentation_covers_previous_allocation =
        widget->presentation_allocation_generation == widget->allocation_generation;
    widget->output_width = output_width;
    widget->output_height = output_height;
    widget->output_scale = scale;
    widget->allocation_metrics_revision = metrics.layout_revision;
    if (allocation_changed) {
        widget->allocation_generation =
            widget->allocation_generation == UINT64_MAX
                ? 1
                : widget->allocation_generation + 1;
        widget->drawn_allocation_generation = 0;
        /* Pixel-only and metric changes need their own native boundary even
         * when the portable grid revision has not changed. */
        if (!surface_resized && !commit_candidate &&
            widget->presentation != NULL && presentation_covers_previous_allocation) {
            widget->presentation_allocation_generation =
                widget->allocation_generation;
        }
    }
    widget->allocated = true;
    widget->allocation_pending = false;
    retain_latest_grid_resize(widget);
    if (surface_resized && widget->presentation != NULL) {
        cairo_surface_t *preview = create_presentation_surface(
            widget,
            output_width,
            output_height);

        /* The renderer already carries the last complete frame at its old
         * coordinates over a background-filled extent. Publish that preview
         * without consuming model dirty while the next frame is pending. */
        if (preview != NULL) {
            g_clear_pointer(&widget->presentation, cairo_surface_destroy);
            widget->presentation = preview;
            widget->presentation_allocation_generation =
                widget->allocation_generation;
            if (widget->presentation_generation != UINT64_MAX) {
                widget->presentation_generation++;
            }
            gtk_widget_queue_draw(gtk_widget);
        }
    }
    if (commit_candidate) {
        cairo_surface_t *candidate = widget->presentation_candidate;

        if (boxrig_pango_cairo_renderer_commit_surface_candidate(
                widget->renderer) == -1) {
            abort_surface_candidate(widget);
            invalidate_complete_presentation(widget);
            return false;
        }
        widget->presentation_candidate = NULL;
        g_clear_pointer(&widget->presentation, cairo_surface_destroy);
        widget->presentation = candidate;
        widget->presentation_allocation_generation =
            widget->allocation_generation;
        widget->surface_candidate_committed = true;
        widget->presentation_composition_revision = widget->surface_candidate_composition_revision;
        if (widget->presentation_generation != UINT64_MAX) {
            widget->presentation_generation++;
        }
        if (consume_presentation_dirty(widget->view) != BOXRIG_UI_OK) {
            invalidate_complete_presentation(widget);
        }
        if (boxrig_ui_view_get_size_command_state(
                widget->view,
                &command_state) == BOXRIG_UI_OK &&
            command_state == BOXRIG_UI_SIZE_COMMAND_IDLE) {
            clear_surface_candidate_state(widget);
        }
        gtk_widget_queue_draw(gtk_widget);
    } else if (!preserve_candidate) {
        invalidate_complete_presentation(widget);
    }
    finish_scrollbar_allocation(widget);
    synchronize_adjustments(widget);
    if (allocation_changed) {
        gtk_widget_queue_draw(gtk_widget);
        request_application_frame(widget);
    }
    return true;
}

static void boxrig_gtk3_terminal_widget_size_allocate(
    GtkWidget *gtk_widget,
    GtkAllocation *allocation)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);

    if (allocation == NULL) {
        return;
    }
    gtk_widget_set_allocation(gtk_widget, allocation);
    if (gtk_widget_get_realized(gtk_widget) && widget->event_window != NULL) {
        gdk_window_move_resize(widget->event_window, allocation->x, allocation->y,
                               allocation->width, allocation->height);
    }
    if (accepts_calls(widget) && !prepare_widget_allocation(widget, allocation)) {
        /* Let the application handle or retry the preparation failure. No
         * native event is fabricated and no self-repeating retry is installed. */
        request_application_frame(widget);
    }
    if (widget->gl_area != NULL) gtk_widget_size_allocate(widget->gl_area, allocation);
}

static gboolean boxrig_gtk3_terminal_widget_draw(
    GtkWidget *gtk_widget,
    cairo_t *cairo)
{
    BoxrigGtk3TerminalWidget *widget =
        BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);
    GdkDrawingContext *drawing_context;

    if (widget->output_scale == 0) {
        return FALSE;
    }
    if ((uses_gl(widget) || widget->gl_presented) && widget->gl_area != NULL) {
        /* GLArea composites with OVER, whereas the retained terminal surface
         * replaces the host background. Clear the draw clip so both renderers
         * preserve the terminal's alpha instead of blending it with the host. */
        cairo_save(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cairo);
        cairo_restore(cairo);
        gtk_container_propagate_draw(GTK_CONTAINER(widget), widget->gl_area, cairo);
    }
    if (!widget->gl_presented && widget->presentation != NULL) {
        cairo_save(cairo);
        cairo_scale(
            cairo,
            1.0 / (double)widget->output_scale,
            1.0 / (double)widget->output_scale);
        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cairo, widget->presentation, 0.0, 0.0);
        cairo_paint(cairo);
        cairo_restore(cairo);
    } else if (!widget->gl_presented) {
        return FALSE;
    }
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        return TRUE;
    }
    drawing_context = gdk_cairo_get_drawing_context(cairo);
    if (accepts_calls(widget) && widget->allocated && !widget->allocation_pending &&
        widget->presentation_visible && gtk_widget_get_mapped(gtk_widget) &&
        drawing_context != NULL &&
        gdk_drawing_context_is_valid(drawing_context) &&
        widget->presentation_allocation_generation ==
            widget->allocation_generation &&
        widget->drawn_allocation_generation != widget->allocation_generation) {
        /* An offscreen draw is not a native allocation boundary. Remember the
         * real boundary independently of when the application pulls the grid;
         * this acknowledges the safe preview, not physical display completion. */
        widget->drawn_allocation_generation = widget->allocation_generation;
        if (widget->pending_grid_resize.revision != 0) {
            request_application_frame(widget);
        }
    }
    return FALSE;
}

/* Native map notifications cannot return an error to the caller. Preserve
 * their failure for the normal frame and diagnostic paths, and wake the host. */
static void set_native_animation_visibility(
    BoxrigGtk3TerminalWidget *widget, bool visible)
{
    boxrig_ui_status status = boxrig_ui_view_set_visible(widget->view, visible);
    if (status != BOXRIG_UI_OK) {
        if (widget->animation_failure == BOXRIG_UI_OK) {
            widget->animation_failure = status;
        }
        request_application_frame(widget);
    }
}

static void boxrig_gtk3_terminal_widget_map(GtkWidget *gtk_widget)
{
    BoxrigGtk3TerminalWidget *widget =
        BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);

    widget->drawn_allocation_generation = 0;
    if (widget->event_window != NULL) {
        gdk_window_show(widget->event_window);
    }
    GTK_WIDGET_CLASS(boxrig_gtk3_terminal_widget_parent_class)->map(gtk_widget);
    boxrig_gtk3_input_map(widget->input);
    if (widget->view != NULL && widget->presentation_visible) {
        set_native_animation_visibility(widget, true);
    }
}

static void boxrig_gtk3_terminal_widget_unmap(GtkWidget *gtk_widget)
{
    BoxrigGtk3TerminalWidget *widget =
        BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);

    widget->drawn_allocation_generation = 0;
    if (widget->view != NULL) {
        set_native_animation_visibility(widget, false);
    }
    boxrig_gtk3_input_unmap(widget->input);
    if (widget->event_window != NULL) {
        gdk_window_hide(widget->event_window);
    }
    GTK_WIDGET_CLASS(boxrig_gtk3_terminal_widget_parent_class)->unmap(gtk_widget);
}

static void boxrig_gtk3_terminal_widget_realize(GtkWidget *gtk_widget)
{
    BoxrigGtk3TerminalWidget *widget =
        BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);
    GtkAllocation allocation;
    GdkWindowAttr attributes;
    GdkWindow *window;

    gtk_widget_set_realized(gtk_widget, true);
    window = gtk_widget_get_parent_window(gtk_widget);
    gtk_widget_set_window(gtk_widget, window);
    g_object_ref(window);
    gtk_widget_get_allocation(gtk_widget, &allocation);
    memset(&attributes, 0, sizeof(attributes));
    attributes.window_type = GDK_WINDOW_CHILD;
    attributes.x = allocation.x;
    attributes.y = allocation.y;
    attributes.width = allocation.width;
    attributes.height = allocation.height;
    attributes.wclass = GDK_INPUT_ONLY;
    attributes.event_mask = gtk_widget_get_events(gtk_widget);
    widget->event_window = gdk_window_new(
        gtk_widget_get_parent_window(gtk_widget),
        &attributes,
        GDK_WA_X | GDK_WA_Y);
    gtk_widget_register_window(gtk_widget, widget->event_window);
    boxrig_gtk3_input_realize(widget->input);
    (void)synchronize_output_scale(widget);
    invalidate_complete_presentation(widget);
    gtk_widget_queue_resize(gtk_widget);
}

static void boxrig_gtk3_terminal_widget_unrealize(GtkWidget *gtk_widget)
{
    BoxrigGtk3TerminalWidget *widget =
        BOXRIG_GTK3_TERMINAL_WIDGET(gtk_widget);

    if (widget->view != NULL) {
        set_native_animation_visibility(widget, false);
    }
    boxrig_gtk3_input_unrealize(widget->input);
    if (widget->event_window != NULL) {
        gtk_widget_unregister_window(gtk_widget, widget->event_window);
        gdk_window_destroy(widget->event_window);
        widget->event_window = NULL;
    }
    GTK_WIDGET_CLASS(boxrig_gtk3_terminal_widget_parent_class)->unrealize(
        gtk_widget);
}

static void boxrig_gtk3_terminal_widget_dispose(GObject *object)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(object);

    if (!widget->disposed) {
        if (widget->gl_area != NULL) gtk_widget_destroy(widget->gl_area);
        if (widget->scroll_container != NULL) {
            g_object_remove_weak_pointer(G_OBJECT(widget->scroll_container), (gpointer *)&widget->scroll_container);
            widget->scroll_container = NULL;
        }
        set_accessibility_busy(widget, false);
        gtk_widget_set_sensitive(GTK_WIDGET(widget), false);
        widget->disposed = true;
        boxrig_gtk3_accessible_detach(widget);
        (void)boxrig_ui_lifecycle_begin_shutdown(&widget->lifecycle);
        widget->frame_request_callback = NULL;
        widget->frame_request_context = NULL;
        boxrig_gtk3_input_destroy(widget->input);
        widget->input = NULL;
        if (widget->view != NULL) {
            (void)boxrig_ui_view_set_frame_request_callback(
                widget->view,
                NULL,
                NULL);
            (void)boxrig_ui_view_set_visible(widget->view, false);
            boxrig_ui_view_release_layout_authority(widget->view);
        }
        if (widget->vertical_adjustment != NULL &&
            widget->vertical_adjustment_handler != 0) {
            g_signal_handler_disconnect(
                widget->vertical_adjustment,
                widget->vertical_adjustment_handler);
            widget->vertical_adjustment_handler = 0;
        }
        (void)boxrig_ui_lifecycle_mark_detached(&widget->lifecycle);
        g_clear_pointer(&widget->presentation, cairo_surface_destroy);
        g_clear_pointer(
            &widget->presentation_candidate,
            cairo_surface_destroy);
        boxrig_pango_cairo_renderer_destroy(widget->renderer);
        widget->renderer = NULL;
        boxrig_ui_animation_runtime_detach(widget->view);
        boxrig_gtk_animation_release(widget->animations);
        widget->animations = NULL;
        boxrig_ui_view_destroy(widget->view);
        widget->view = NULL;
        font_specification_storage_destroy(&widget->font);
        boxrig_font_visual_step_sequence_destroy(widget->visual_steps);
        widget->visual_steps = NULL;
        widget->model = NULL;
        g_clear_object(&widget->horizontal_adjustment);
        g_clear_object(&widget->vertical_adjustment);
        (void)boxrig_ui_lifecycle_mark_stopped(&widget->lifecycle);
    }
    G_OBJECT_CLASS(boxrig_gtk3_terminal_widget_parent_class)->dispose(object);
}

static void boxrig_gtk3_terminal_widget_finalize(GObject *object)
{
    BoxrigGtk3TerminalWidget *widget = BOXRIG_GTK3_TERMINAL_WIDGET(object);

    g_clear_pointer(&widget->main_context, g_main_context_unref);
    G_OBJECT_CLASS(boxrig_gtk3_terminal_widget_parent_class)->finalize(object);
}

static void boxrig_gtk3_terminal_widget_class_init(
    BoxrigGtk3TerminalWidgetClass *widget_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(widget_class);
    GtkWidgetClass *gtk_widget_class = GTK_WIDGET_CLASS(widget_class);
    GtkContainerClass *container_class = GTK_CONTAINER_CLASS(widget_class);

    container_class->forall = terminal_forall;
    container_class->remove = terminal_remove;

    object_class->get_property = boxrig_gtk3_terminal_widget_get_property;
    object_class->set_property = boxrig_gtk3_terminal_widget_set_property;
    object_class->dispose = boxrig_gtk3_terminal_widget_dispose;
    object_class->finalize = boxrig_gtk3_terminal_widget_finalize;
    gtk_widget_class->get_preferred_width =
        boxrig_gtk3_terminal_widget_get_preferred_width;
    gtk_widget_class->get_preferred_height =
        boxrig_gtk3_terminal_widget_get_preferred_height;
    gtk_widget_class->size_allocate =
        boxrig_gtk3_terminal_widget_size_allocate;
    gtk_widget_class->draw = boxrig_gtk3_terminal_widget_draw;
    gtk_widget_class->map = boxrig_gtk3_terminal_widget_map;
    gtk_widget_class->unmap = boxrig_gtk3_terminal_widget_unmap;
    gtk_widget_class->realize = boxrig_gtk3_terminal_widget_realize;
    gtk_widget_class->unrealize = boxrig_gtk3_terminal_widget_unrealize;
    gtk_widget_class_set_accessible_type(
        gtk_widget_class,
        boxrig_gtk3_terminal_accessible_get_type());
    gtk_widget_class_set_accessible_role(
        gtk_widget_class,
        ATK_ROLE_TERMINAL);
    gtk_widget_class_set_css_name(gtk_widget_class, "boxrig-terminal");
    g_object_class_override_property(
        object_class,
        PROP_HADJUSTMENT,
        "hadjustment");
    g_object_class_override_property(
        object_class,
        PROP_VADJUSTMENT,
        "vadjustment");
    g_object_class_override_property(
        object_class,
        PROP_HSCROLL_POLICY,
        "hscroll-policy");
    g_object_class_override_property(
        object_class,
        PROP_VSCROLL_POLICY,
        "vscroll-policy");
}

static void boxrig_gtk3_terminal_widget_init(
    BoxrigGtk3TerminalWidget *widget)
{
    widget->main_context = g_main_context_ref_thread_default();
    widget->owner_thread = g_thread_self();
    widget->window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    widget->horizontal_policy = GTK_SCROLL_MINIMUM;
    widget->vertical_policy = GTK_SCROLL_MINIMUM;
    widget->output_scale = 1;
    boxrig_ui_lifecycle_initialize(&widget->lifecycle);
    replace_horizontal_adjustment(widget, NULL);
    replace_vertical_adjustment(widget, NULL);
    gtk_widget_set_can_focus(GTK_WIDGET(widget), true);
    gtk_widget_set_has_window(GTK_WIDGET(widget), false);
    g_signal_connect(
        widget,
        "notify::scale-factor",
        G_CALLBACK(scale_factor_changed),
        widget);
}

static void boxrig_gtk3_terminal_widget_scrollable_init(
    GtkScrollableInterface *interface)
{
    (void)interface;
}

boxrig_gtk3_terminal_widget_config
boxrig_gtk3_terminal_widget_config_default(void)
{
    boxrig_gtk3_terminal_widget_config config;

    config.view = boxrig_ui_view_config_default();
    config.attachment = BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    config.paste_capacity = BOXRIG_GTK3_DEFAULT_PASTE_CAPACITY;
    config.paste_chunk_size = BOXRIG_GTK3_DEFAULT_PASTE_CHUNK_SIZE;
    config.maximum_native_input_events =
        BOXRIG_GTK3_NATIVE_INPUT_EVENTS_DEFAULT;
    config.maximum_frame_items = BOXRIG_GTK3_FRAME_ITEMS_DEFAULT;
    config.maximum_clipboard_transfers =
        BOXRIG_GTK3_DEFAULT_CLIPBOARD_TRANSFERS;
    config.maximum_clipboard_bytes = BOXRIG_GTK3_DEFAULT_CLIPBOARD_BYTES;
    config.transparency = boxrig_ui_transparency_config_default();
    config.render_policy = BOXRIG_GTK_RENDER_CAIRO;
    config.ime_policy = BOXRIG_GTK_IME_AUTO;
    config.candidate_policy = BOXRIG_UI_CANDIDATE_AUTO;
    config.accessibility_enabled = false;
    config.frame_request_callback = NULL;
    config.frame_request_context = NULL;
    config.initial_font = NULL;
    return config;
}

static boxrig_ui_status initialize_widget_resources(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_gtk3_terminal_widget_config *config)
{
    boxrig_font_specification default_specification;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state state;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;
    boxrig_gtk3_input_config input_config;
    int scale;

    default_specification = config->initial_font != NULL
        ? *config->initial_font : boxrig_font_specification_default();
    widget->rendering_policy = boxrig_font_rendering_policy_default();
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    if (scale <= 0) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    widget->rendering_policy.output_scale = (float)scale;
    widget->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    if (!font_specification_storage_copy(
            &default_specification,
            &widget->font)) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (boxrig_pango_cairo_renderer_create(
            1,
            1,
            &widget->renderer) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (config->initial_font != NULL || scale != 1) {
        font_status = boxrig_pango_cairo_renderer_set_font(
            widget->renderer,
            &widget->font.specification,
            &widget->rendering_policy);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return status_from_font_catalog(font_status);
        }
    }
    font_status = boxrig_pango_cairo_renderer_get_effective_metrics(
        widget->renderer,
        &metrics);
    if (font_status != BOXRIG_FONT_CATALOG_OK) {
        return status_from_font_catalog(font_status);
    }
    status = boxrig_ui_view_create(
        widget->model,
        &config->view,
        &widget->view);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_gtk_animation_acquire(widget->main_context, &widget->animations);
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_animation_runtime_attach(
            boxrig_gtk_animation_runtime(widget->animations), widget->view);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_require_animation_runtime(widget->view);
    }
    if (status != BOXRIG_UI_OK) return status;
    if (config->attachment == BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY) {
        status = boxrig_ui_view_acquire_layout_authority(widget->view);
        if (status == BOXRIG_UI_OK) {
            status = boxrig_ui_view_commit_cell_metrics(
                widget->view,
                &metrics);
        }
    } else {
        status = boxrig_ui_view_validate_cell_metrics(widget->view, &metrics);
    }
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    status = boxrig_ui_view_set_geometry_capabilities(
        widget->view,
        BOXRIG_UI_GEOMETRY_SIZE_COMMAND);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target = true;
    capabilities.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    status = boxrig_ui_view_report_transparency_capabilities(
        widget->view,
        &capabilities);
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_set_transparency_config(
            widget->view,
            &config->transparency);
    }
    if (status == BOXRIG_UI_OK) {
        status = boxrig_ui_view_get_transparency(widget->view, &state);
    }
    if (status != BOXRIG_UI_OK ||
        boxrig_pango_cairo_renderer_apply_transparency_state(
            widget->renderer,
            &state) == -1 ||
        boxrig_ui_view_set_visible(widget->view, false) != BOXRIG_UI_OK) {
        return status == BOXRIG_UI_OK ? BOXRIG_UI_UNAVAILABLE : status;
    }
    memset(&input_config, 0, sizeof(input_config));
    input_config.widget = GTK_WIDGET(widget);
    input_config.ime_policy = config->ime_policy;
    input_config.candidate_policy = config->candidate_policy;
    input_config.terminal = boxrig_ui_model_terminal(widget->model);
    input_config.view = widget->view;
    input_config.paste_capacity = config->paste_capacity;
    input_config.paste_chunk_size = config->paste_chunk_size;
    input_config.maximum_events = config->maximum_native_input_events;
    input_config.maximum_clipboard_transfers =
        config->maximum_clipboard_transfers;
    input_config.maximum_clipboard_bytes = config->maximum_clipboard_bytes;
    input_config.activity_callback = input_activity;
    input_config.activity_context = widget;
    status = boxrig_gtk3_input_create(&input_config, &widget->input);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    return boxrig_gtk3_terminal_widget_set_frame_request_callback(
        widget,
        config->frame_request_callback,
        config->frame_request_context);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_create(
    boxrig_ui_model *model,
    const boxrig_gtk3_terminal_widget_config *config,
    BoxrigGtk3TerminalWidget **widget_out)
{
    boxrig_gtk_render_state state;
    return boxrig_gtk3_terminal_widget_create_with_render_state(model, config, widget_out, &state);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_create_with_render_state(
    boxrig_ui_model *model,
    const boxrig_gtk3_terminal_widget_config *config,
    BoxrigGtk3TerminalWidget **widget_out,
    boxrig_gtk_render_state *state_out)
{
    BoxrigGtk3TerminalWidget *widget;
    boxrig_ui_status status;

    if (widget_out != NULL) *widget_out = NULL;
    if (state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    memset(state_out, 0, sizeof(*state_out));
    state_out->requested_policy = config == NULL ? BOXRIG_GTK_RENDER_CAIRO : config->render_policy;
    state_out->status = BOXRIG_UI_INVALID_ARGUMENT;
    if (model == NULL || config == NULL || widget_out == NULL ||
        config->render_policy < BOXRIG_GTK_RENDER_CAIRO ||
        config->render_policy > BOXRIG_GTK_RENDER_OPENGL_REQUIRED ||
        boxrig_ui_model_terminal(model) == NULL ||
        !valid_attachment(config->attachment) ||
        (config->initial_font != NULL &&
         boxrig_font_specification_validate(NULL, config->initial_font) != BOXRIG_FONT_CATALOG_OK) ||
        config->paste_capacity == 0 ||
        config->paste_capacity > BOXRIG_GTK3_MAXIMUM_PASTE_CAPACITY ||
        config->paste_chunk_size == 0 ||
        config->paste_chunk_size > BOXRIG_GTK3_MAXIMUM_PASTE_CHUNK_SIZE ||
        config->maximum_native_input_events == 0 ||
        config->maximum_native_input_events >
            BOXRIG_GTK3_NATIVE_INPUT_EVENTS_MAXIMUM ||
        config->maximum_frame_items == 0 ||
        config->maximum_frame_items > BOXRIG_GTK3_FRAME_ITEMS_MAXIMUM ||
        config->maximum_clipboard_transfers == 0 ||
        config->maximum_clipboard_transfers >
            BOXRIG_GTK3_MAXIMUM_CLIPBOARD_TRANSFERS ||
        config->maximum_clipboard_bytes == 0 ||
        config->maximum_clipboard_bytes >
            BOXRIG_GTK3_MAXIMUM_CLIPBOARD_BYTES ||
        ((config->frame_request_callback == NULL) !=
         (config->frame_request_context == NULL))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    state_out->status = BOXRIG_UI_OK;
    state_out->reason = BOXRIG_GTK_RENDER_REASON_REQUESTED;
#if !BOXRIG_GTK_OPENGL
    if (config->render_policy != BOXRIG_GTK_RENDER_CAIRO) {
        state_out->status = BOXRIG_UI_UNAVAILABLE;
        state_out->reason = BOXRIG_GTK_RENDER_REASON_NOT_BUILT;
        snprintf(state_out->message, sizeof(state_out->message), "OpenGL support was not built");
        if (config->render_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED) return BOXRIG_UI_UNAVAILABLE;
    }
#endif
    widget = g_object_new(BOXRIG_GTK3_TYPE_TERMINAL_WIDGET, NULL);
    if (widget == NULL) {
        state_out->status = BOXRIG_UI_OUT_OF_MEMORY;
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    widget->render_state = *state_out;
    widget->gl_failed = state_out->reason == BOXRIG_GTK_RENDER_REASON_NOT_BUILT;
    widget->model = model;
    widget->accessibility_enabled = config->accessibility_enabled;
    widget->maximum_native_input_events =
        config->maximum_native_input_events;
    widget->maximum_frame_items = config->maximum_frame_items;
    status = initialize_widget_resources(widget, config);
    if (status != BOXRIG_UI_OK) {
        state_out->status = status;
        state_out->reason = BOXRIG_GTK_RENDER_REASON_RESOURCE;
        g_object_ref_sink(widget);
        g_object_unref(widget);
        return status;
    }
    widget->render_state.pending = true;
    state_out->pending = true;
#if BOXRIG_GTK_OPENGL
    if (uses_gl(widget)) {
        create_gl_area(widget);
    }
#endif
    *widget_out = widget;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_ime_state(
    const BoxrigGtk3TerminalWidget *widget, boxrig_gtk_ime_state *state_out)
{
    if (!accepts_calls(widget) || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_gtk3_input_get_ime_state(widget->input, state_out);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_render_state(
    const BoxrigGtk3TerminalWidget *widget, boxrig_gtk_render_state *state_out)
{
    if (!accepts_calls(widget) || state_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *state_out = widget->render_state;
    return BOXRIG_UI_OK;
}

bool boxrig_gtk3_terminal_widget_get_accessibility_enabled(
    const BoxrigGtk3TerminalWidget *widget)
{
    return widget != NULL && widget->accessibility_enabled;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_accessibility_enabled(
    BoxrigGtk3TerminalWidget *widget, bool enabled)
{
    if (!accepts_calls(widget)) return BOXRIG_UI_INVALID_ARGUMENT;
    if (widget->in_frame || widget->in_gl_render) return BOXRIG_UI_BUSY;
    if (widget->accessibility_enabled == enabled) return BOXRIG_UI_OK;
    widget->accessibility_enabled = enabled;
    if (enabled) {
        request_application_frame(widget);
        return BOXRIG_UI_OK;
    }
    /* Withdrawal signals can destroy the widget or attempt a nested frame. */
    g_object_ref(widget);
    widget->in_frame = true;
    boxrig_ui_status status = boxrig_gtk3_accessible_update(widget);
    widget->in_frame = false;
    g_object_unref(widget);
    return status;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_render_policy(
    BoxrigGtk3TerminalWidget *widget, boxrig_gtk_render_policy policy)
{
    if (!accepts_calls(widget) || policy < BOXRIG_GTK_RENDER_CAIRO ||
        policy > BOXRIG_GTK_RENDER_OPENGL_REQUIRED) return BOXRIG_UI_INVALID_ARGUMENT;
    if (widget->in_frame || widget->in_gl_render) return BOXRIG_UI_BUSY;
    bool already_active = widget->render_state.active_backend == (policy == BOXRIG_GTK_RENDER_CAIRO
            ? BOXRIG_GTK_RENDER_BACKEND_CAIRO : BOXRIG_GTK_RENDER_BACKEND_OPENGL) &&
        (policy == BOXRIG_GTK_RENDER_CAIRO || (!widget->gl_failed && widget->gl_presented));
    widget->render_state.requested_policy = policy;
    widget->render_state.status = BOXRIG_UI_OK;
    widget->render_state.reason = BOXRIG_GTK_RENDER_REASON_REQUESTED;
    widget->render_state.gl_code = 0;
    widget->render_state.reset_status = 0;
    widget->render_state.message[0] = '\0';
    widget->render_state.pending = !already_active;
    widget->gl_failed = false;
    widget->cairo_failed = false;
    if (already_active) {
#if BOXRIG_GTK_OPENGL
        if (policy == BOXRIG_GTK_RENDER_CAIRO) release_gl_area(widget);
#endif
        return BOXRIG_UI_OK;
    }
#if BOXRIG_GTK_OPENGL
    if (policy != BOXRIG_GTK_RENDER_CAIRO) {
        release_gl_area(widget);
        widget->gl_desktop_only = false;
        widget->gl_retry_desktop = false;
        create_gl_area(widget);
    }
#else
    if (policy != BOXRIG_GTK_RENDER_CAIRO) {
        widget->gl_failed = true;
        widget->render_state.status = BOXRIG_UI_UNAVAILABLE;
        widget->render_state.reason = BOXRIG_GTK_RENDER_REASON_NOT_BUILT;
        widget->render_state.pending = widget->render_state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE &&
            policy == BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
        snprintf(widget->render_state.message, sizeof(widget->render_state.message), "OpenGL support was not built");
    }
#endif
    invalidate_complete_presentation(widget);
    request_application_frame(widget);
#if !BOXRIG_GTK_OPENGL
    if (widget->gl_failed) return BOXRIG_UI_UNAVAILABLE;
#endif
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_quiesce(
    BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_lifecycle_state state;

    if (widget == NULL || widget->owner_thread != g_thread_self()) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    state = boxrig_ui_lifecycle_get_state(&widget->lifecycle);
    if (state == BOXRIG_UI_LIFECYCLE_RUNNING &&
        boxrig_ui_lifecycle_begin_shutdown(&widget->lifecycle) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_scrollbar_visibility_cancel(&widget->scrollbar_visibility);
    widget->scrollbar_native_pending = false;
    boxrig_ui_animation_runtime_detach(widget->view);
    boxrig_gtk_animation_release(widget->animations);
    widget->animations = NULL;
    widget->frame_request_callback = NULL;
    widget->frame_request_context = NULL;
    memset(&widget->pending_grid_resize, 0, sizeof(widget->pending_grid_resize));
    memset(&widget->dispatched_grid_resize, 0, sizeof(widget->dispatched_grid_resize));
    set_accessibility_busy(widget, false);
    gtk_widget_set_sensitive(GTK_WIDGET(widget), false);
    boxrig_gtk3_input_quiesce(widget->input);
    if (widget->view != NULL) {
        (void)boxrig_ui_view_set_frame_request_callback(
            widget->view,
            NULL,
            NULL);
        if (boxrig_ui_view_set_visible(widget->view, false) != BOXRIG_UI_OK) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        boxrig_ui_view_release_layout_authority(widget->view);
    }
    widget->presentation_visible = false;
    return BOXRIG_UI_OK;
}

boxrig_ui_view *boxrig_gtk3_terminal_widget_view(
    BoxrigGtk3TerminalWidget *widget)
{
    return widget == NULL ? NULL : widget->view;
}

boxrig_ui_model *boxrig_gtk3_terminal_widget_model(
    BoxrigGtk3TerminalWidget *widget)
{
    return widget == NULL ? NULL : widget->model;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_window_state(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_window_state window_state)
{
    if (!accepts_calls(widget) || !valid_window_state(window_state)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    widget->window_state = window_state;
    gtk_widget_queue_allocate(GTK_WIDGET(widget));
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_geometry_hints(
    BoxrigGtk3TerminalWidget *widget,
    GtkWindow *window,
    bool enabled)
{
    boxrig_font_effective_metrics metrics;
    GdkGeometry geometry;
    unsigned int scale;
    unsigned int cell_width;
    unsigned int cell_height;

    if (!accepts_calls(widget) || !GTK_IS_WINDOW(window)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&geometry, 0, sizeof(geometry));
    if (!enabled || widget->window_state != BOXRIG_UI_WINDOW_NORMAL) {
        gtk_window_set_geometry_hints(window, NULL, &geometry, 0);
        return BOXRIG_UI_OK;
    }
    if (boxrig_pango_cairo_renderer_get_effective_metrics(
            widget->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    if (scale == 0) {
        scale = 1;
    }
    cell_width = metrics.cell_width / scale +
                 (metrics.cell_width % scale == 0 ? 0U : 1U);
    cell_height = metrics.cell_height / scale +
                  (metrics.cell_height % scale == 0 ? 0U : 1U);
    if (cell_width == 0 || cell_height == 0 || cell_width > INT_MAX ||
        cell_height > INT_MAX) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    geometry.min_width = (int)cell_width;
    geometry.min_height = (int)cell_height;
    geometry.base_width = (int)cell_width;
    geometry.base_height = (int)cell_height;
    geometry.width_inc = (int)cell_width;
    geometry.height_inc = (int)cell_height;
    gtk_window_set_geometry_hints(
        window,
        GTK_WIDGET(widget),
        &geometry,
        GDK_HINT_MIN_SIZE | GDK_HINT_BASE_SIZE | GDK_HINT_RESIZE_INC);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_layout(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_layout_result *layout_out)
{
    if (widget == NULL || widget->view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_layout(widget->view, layout_out);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_preferred_grid(
    BoxrigGtk3TerminalWidget *widget,
    size_t rows,
    size_t columns)
{
    boxrig_ui_status status;

    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_set_preferred_grid(widget->view, rows, columns);
    if (status == BOXRIG_UI_OK) {
        gtk_widget_queue_resize(GTK_WIDGET(widget));
    }
    return status;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_preferred_grid(
    const BoxrigGtk3TerminalWidget *widget,
    size_t *rows_out,
    size_t *columns_out)
{
    if (widget == NULL || widget->view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_preferred_grid(
        widget->view,
        rows_out,
        columns_out);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_size_command_state(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_size_command_state *state_out)
{
    return widget == NULL || widget->view == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : boxrig_ui_view_get_size_command_state(
                     widget->view,
                     state_out);
}

bool boxrig_gtk3_terminal_widget_take_layout_action(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_layout_action *action_out)
{
    boxrig_ui_size_command_state state;
    boxrig_ui_layout_action action;
    boxrig_ui_layout_result layout;
    bool candidate_required;

    if (!accepts_calls(widget) || action_out == NULL ||
        boxrig_ui_view_get_size_command_state(widget->view, &state) !=
            BOXRIG_UI_OK) {
        return false;
    }
    if (state == BOXRIG_UI_SIZE_COMMAND_READY) {
        if (!boxrig_ui_view_peek_layout_action(widget->view, &action) ||
            action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            return false;
        }
        candidate_required =
            widget->zoom_generation_active &&
            action.value.content_size.cause ==
                BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
        if ((candidate_required &&
             (!widget->surface_candidate_ready ||
              boxrig_ui_view_get_layout(widget->view, &layout) !=
                  BOXRIG_UI_OK ||
              widget->surface_candidate_width != layout.grid_width ||
              widget->surface_candidate_height != layout.grid_height)) ||
            !boxrig_ui_view_take_layout_action(widget->view, &action) ||
            action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            return false;
        }
        if (candidate_required) {
            widget->surface_candidate_revision = action.revision;
        }
        *action_out = action;
        return true;
    }
    if (state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED || !widget->allocated) {
        return false;
    }
    retain_latest_grid_resize(widget);
    if (!grid_resize_ready(widget)) {
        return false;
    }
    action_out->kind = BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE;
    action_out->revision = widget->pending_grid_resize.revision;
    action_out->value.grid_resize.rows = widget->pending_grid_resize.rows;
    action_out->value.grid_resize.columns =
        widget->pending_grid_resize.columns;
    widget->dispatched_grid_resize = widget->pending_grid_resize;
    memset(&widget->pending_grid_resize, 0, sizeof(widget->pending_grid_resize));
    return true;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_complete_size_command(
    BoxrigGtk3TerminalWidget *widget,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    boxrig_ui_status status;

    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_complete_size_command(
        widget->view,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
    if (status == BOXRIG_UI_OK && applied_out != NULL && *applied_out &&
        widget->surface_candidate_ready) {
        if (widget->surface_candidate_committed) {
            clear_surface_candidate_state(widget);
        } else if (outcome != BOXRIG_UI_SIZE_ACCEPTED ||
                   revision != widget->surface_candidate_revision ||
                   allocation_width != widget->surface_candidate_width ||
                   allocation_height != widget->surface_candidate_height) {
            abort_surface_candidate(widget);
            invalidate_complete_presentation(widget);
        }
    }
    return status;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_complete_grid_resize(
    BoxrigGtk3TerminalWidget *widget,
    uint64_t revision,
    bool succeeded)
{
    boxrig_terminal *terminal;
    size_t rows;
    size_t columns;

    if (!accepts_calls(widget) || revision == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (widget->dispatched_grid_resize.revision != revision) {
        return BOXRIG_UI_STALE;
    }
    if (!succeeded) {
        /* Transport may already have changed. Stop without inventing rollback
         * or turning an application session failure into an input failure. */
        return boxrig_gtk3_terminal_widget_quiesce(widget);
    }
    terminal = boxrig_ui_model_terminal(widget->model);
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows != widget->dispatched_grid_resize.rows ||
        columns != widget->dispatched_grid_resize.columns) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&widget->dispatched_grid_resize, 0, sizeof(widget->dispatched_grid_resize));
    apply_scrollbar_native_policy(widget);
    invalidate_complete_presentation(widget);
    request_application_frame(widget);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_font_specification(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_specification *specification)
{
    if (widget == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return set_widget_font(
        widget,
        specification,
        &widget->rendering_policy,
        BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS,
        true);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_font_specification(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_specification *specification_out)
{
    if (widget == NULL || widget->view == NULL ||
        specification_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *specification_out = widget->font.specification;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_font_rendering_policy(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_rendering_policy *policy)
{
    if (widget == NULL || policy == NULL ||
        policy->output_scale !=
            (float)gtk_widget_get_scale_factor(GTK_WIDGET(widget))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return set_widget_font(
        widget,
        &widget->font.specification,
        policy,
        BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS,
        true);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_font_rendering_policy(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_rendering_policy *policy_out)
{
    if (widget == NULL || widget->view == NULL || policy_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *policy_out = widget->rendering_policy;
    return BOXRIG_UI_OK;
}

boxrig_font_catalog_status
boxrig_gtk3_terminal_widget_get_effective_metrics(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_effective_metrics *metrics_out)
{
    if (widget == NULL || widget->renderer == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_pango_cairo_renderer_get_effective_metrics(
        widget->renderer,
        metrics_out);
}

boxrig_font_catalog_status boxrig_gtk3_terminal_widget_register_font_file(
    BoxrigGtk3TerminalWidget *widget,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (!accepts_calls(widget)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_pango_cairo_renderer_register_font_file(
        widget->renderer,
        path,
        path_length,
        face_index,
        resource_id_out);
}

boxrig_font_catalog_status boxrig_gtk3_terminal_widget_register_font_memory(
    BoxrigGtk3TerminalWidget *widget,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    if (!accepts_calls(widget)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return boxrig_pango_cairo_renderer_register_font_memory(
        widget->renderer,
        data,
        data_length,
        face_index,
        resource_id_out);
}

boxrig_font_resource_id
boxrig_gtk3_terminal_widget_primary_font_resource(
    const BoxrigGtk3TerminalWidget *widget)
{
    return boxrig_pango_cairo_renderer_primary_font_resource(
        widget == NULL ? NULL : widget->renderer);
}

uint64_t boxrig_gtk3_terminal_widget_font_catalog_revision(
    const BoxrigGtk3TerminalWidget *widget)
{
    return boxrig_pango_cairo_renderer_font_catalog_revision(
        widget == NULL ? NULL : widget->renderer);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_line_height_adjustment(
    BoxrigGtk3TerminalWidget *widget,
    float adjustment_dip)
{
    boxrig_font_specification specification;

    if (widget == NULL || widget->view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    specification = widget->font.specification;
    specification.line_height_adjustment_dip = adjustment_dip;
    return set_widget_font(
        widget,
        &specification,
        &widget->rendering_policy,
        BOXRIG_UI_LAYOUT_CHANGE_LINE_HEIGHT,
        true);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_line_height_adjustment(
    const BoxrigGtk3TerminalWidget *widget,
    float *adjustment_dip_out)
{
    if (widget == NULL || widget->view == NULL ||
        adjustment_dip_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *adjustment_dip_out =
        widget->font.specification.line_height_adjustment_dip;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_visual_zoom(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    boxrig_font_visual_step_sequence *sequence = NULL;
    boxrig_font_effective_metrics current;
    boxrig_font_effective_metrics candidate;
    boxrig_font_catalog_status font_status;
    boxrig_ui_status status;

    if (!accepts_calls(widget) || changed_out == NULL ||
        (direction != BOXRIG_FONT_ZOOM_DECREASE &&
         direction != BOXRIG_FONT_ZOOM_INCREASE)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (widget->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    if (widget->visual_steps == NULL ||
        !visual_step_policy_matches(widget, policy)) {
        font_status = boxrig_pango_cairo_renderer_get_effective_metrics(
            widget->renderer,
            &current);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return status_from_font_catalog(font_status);
        }
        font_status = boxrig_font_visual_step_sequence_create(
            &current,
            widget->font.specification.line_height_adjustment_dip,
            policy,
            resolve_widget_zoom_metrics,
            widget,
            &sequence);
        if (font_status != BOXRIG_FONT_CATALOG_OK) {
            return status_from_font_catalog(font_status);
        }
        boxrig_font_visual_step_sequence_destroy(widget->visual_steps);
        widget->visual_steps = sequence;
        widget->visual_step_policy = policy;
        widget->visual_step_policy_valid = true;
    }
    if (!boxrig_font_visual_step_sequence_peek(
            widget->visual_steps,
            direction,
            &candidate)) {
        return BOXRIG_UI_OK;
    }
    status = queue_or_commit_widget_zoom(widget, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_advance(
        widget->visual_steps,
        direction);
    *changed_out = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_visual_zoom_reset(
    BoxrigGtk3TerminalWidget *widget,
    bool *changed_out)
{
    boxrig_font_effective_metrics candidate;
    boxrig_ui_status status;

    if (!accepts_calls(widget) || changed_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (widget->scrollbar_visibility.state.pending) return BOXRIG_UI_BUSY;
    if (!boxrig_font_visual_step_sequence_peek_reset(
            widget->visual_steps,
            &candidate)) {
        return BOXRIG_UI_OK;
    }
    status = queue_or_commit_widget_zoom(widget, &candidate);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_reset(widget->visual_steps);
    *changed_out = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_text_direction(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_text_direction_policy policy)
{
    if (!accepts_calls(widget) ||
        boxrig_pango_cairo_renderer_set_direction_policy(
            widget->renderer,
            policy) == -1) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (widget->direction_policy != policy) {
        widget->direction_policy = policy;
        invalidate_complete_presentation(widget);
    }
    return BOXRIG_UI_OK;
}

boxrig_text_direction_policy boxrig_gtk3_terminal_widget_text_direction(
    const BoxrigGtk3TerminalWidget *widget)
{
    return widget == NULL ? BOXRIG_TEXT_DIRECTION_AUTO
                          : widget->direction_policy;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_frame_request_callback(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    boxrig_ui_status status;

    if (!accepts_calls(widget) ||
        ((callback == NULL) != (context == NULL))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_set_frame_request_callback(
        widget->view,
        callback,
        context);
    if (status == BOXRIG_UI_OK) {
        widget->frame_request_callback = callback;
        widget->frame_request_context = context;
    }
    return status;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_input_enabled(
    BoxrigGtk3TerminalWidget *widget, bool enabled)
{
    if (!accepts_calls(widget)) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_gtk3_input_set_enabled(widget->input, enabled);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_input_state(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_input_state *state_out)
{
    if (!accepts_calls(widget)) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_gtk3_input_get_state(widget->input, state_out);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_input_config(
    BoxrigGtk3TerminalWidget *widget, const boxrig_input_config *config)
{
    if (!accepts_calls(widget)) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_gtk3_input_set_config(widget->input, config);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_key_router(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_input_key_router router,
    void *context)
{
    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_gtk3_input_set_key_router(
        widget->input,
        router,
        context);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_mouse_router(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_input_mouse_router router,
    void *context)
{
    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_gtk3_input_set_mouse_router(
        widget->input,
        router,
        context);
}

boxrig_ui_view_action_capabilities
boxrig_gtk3_terminal_widget_view_action_capabilities(
    const BoxrigGtk3TerminalWidget *widget)
{
    return boxrig_gtk3_input_view_action_capabilities(
        widget == NULL ? NULL : widget->input);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_apply_view_action(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_ui_view_action *action)
{
    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_gtk3_input_apply_view_action(
        widget->input,
        widget->view,
        action);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_queue_paste(
    BoxrigGtk3TerminalWidget *widget,
    const uint8_t *bytes,
    size_t length)
{
    boxrig_ui_status status;
    bool input_blocked;
    bool additional_dispatch;

    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (widget->in_frame || widget->in_gl_render) {
        return BOXRIG_UI_BUSY;
    }
    widget->in_frame = true;
    retain_latest_grid_resize(widget);
    status = boxrig_gtk3_input_queue_paste(widget->input, bytes, length);
    if (status == BOXRIG_UI_OK) {
        status = boxrig_gtk3_input_frame(
            widget->input,
            widget->maximum_frame_items,
            input_geometry_ready(widget),
            &input_blocked,
            &additional_dispatch);
    }
    widget->in_frame = false;
    return status;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_hit_test(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out)
{
    if (widget == NULL || widget->view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_hit_test_with_composition(
        widget->view,
        boxrig_gtk3_input_composition(widget->input),
        x,
        y,
        result_out);
}

boxrig_composition *boxrig_gtk3_terminal_widget_composition(
    BoxrigGtk3TerminalWidget *widget)
{
    return boxrig_gtk3_input_composition(
        widget == NULL ? NULL : widget->input);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_visible(
    BoxrigGtk3TerminalWidget *widget,
    bool visible)
{
    boxrig_ui_status status;
    bool view_visible;

    if (!accepts_calls(widget)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    view_visible = visible && gtk_widget_get_mapped(GTK_WIDGET(widget));
    status = boxrig_ui_view_set_visible(widget->view, view_visible);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    widget->presentation_visible = visible;
    gtk_widget_set_visible(GTK_WIDGET(widget), visible);
    return BOXRIG_UI_OK;
}

bool boxrig_gtk3_terminal_widget_visible(
    const BoxrigGtk3TerminalWidget *widget)
{
    return widget != NULL && widget->presentation_visible;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_scrollbar_state(
    BoxrigGtk3TerminalWidget *widget, boxrig_scrollbar_state *state_out)
{
    return widget == NULL ? BOXRIG_UI_INVALID_ARGUMENT
        : boxrig_ui_view_get_scrollbar_state(widget->view, state_out);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_scrollbar_model(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_scrollbar_model *model_out)
{
    if (widget == NULL || widget->model == NULL || model_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_scrollbar_model_read(
               boxrig_ui_model_terminal(widget->model),
               model_out) == 0
               ? BOXRIG_UI_OK
               : BOXRIG_UI_UNAVAILABLE;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_apply_scrollbar_action(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    if (!(accepts_calls(widget))) return BOXRIG_UI_INVALID_ARGUMENT;
    return boxrig_ui_view_apply_scrollbar_action(widget->view, action, position, changed_out);
}

boxrig_ui_status boxrig_gtk3_terminal_widget_set_transparency(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_state previous;
    boxrig_ui_transparency_state candidate;
    boxrig_ui_status status;

    if (!accepts_calls(widget) || config == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_view_get_transparency(widget->view, &previous);
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
    if (boxrig_pango_cairo_renderer_apply_transparency_state(
            widget->renderer,
            &candidate) == -1) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_ui_view_set_transparency_config(widget->view, config);
    if (status != BOXRIG_UI_OK) {
        (void)boxrig_pango_cairo_renderer_apply_transparency_state(
            widget->renderer,
            &previous);
    }
    return status;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_transparency(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_transparency_state *state_out)
{
    if (widget == NULL || widget->view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_ui_view_get_transparency(widget->view, state_out);
}

uint32_t boxrig_gtk3_terminal_widget_capabilities(
    const BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_transparency_state transparency;
    uint32_t capabilities;

    if (widget == NULL || widget->view == NULL) {
        return 0;
    }
    capabilities = BOXRIG_UI_COMPONENT_CAPABILITY_OWNED_VISIBLE_CHILD |
                   BOXRIG_UI_COMPONENT_CAPABILITY_VIEW_DIRTY |
                   BOXRIG_UI_COMPONENT_CAPABILITY_FONT_RESOURCES |
                   BOXRIG_UI_COMPONENT_CAPABILITY_INPUT_METHOD |
                   BOXRIG_UI_COMPONENT_CAPABILITY_CLIPBOARD_PASTE |
                   BOXRIG_UI_COMPONENT_CAPABILITY_SIZE_COMMAND;
    if (boxrig_ui_view_get_transparency(
            widget->view,
            &transparency) == BOXRIG_UI_OK &&
        transparency.capabilities.premultiplied_argb32_target) {
        capabilities |= BOXRIG_UI_COMPONENT_CAPABILITY_TARGET_ALPHA;
    }
    return capabilities;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_get_diagnostic(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_state transparency;
    boxrig_gtk3_input_diagnostic input_diagnostic;

    if (widget == NULL || diagnostic_out == NULL || widget->view == NULL ||
        widget->renderer == NULL ||
        boxrig_pango_cairo_renderer_get_effective_metrics(
            widget->renderer,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_ui_view_get_transparency(
            widget->view,
            &transparency) != BOXRIG_UI_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    boxrig_gtk3_input_get_diagnostic(widget->input, &input_diagnostic);
    diagnostic_out->running =
        boxrig_ui_lifecycle_accepts_events(&widget->lifecycle) &&
        input_diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_NONE;
    diagnostic_out->visible = widget->presentation_visible;
    diagnostic_out->focused = gtk_widget_has_focus(GTK_WIDGET(widget));
    diagnostic_out->input_blocked = input_diagnostic.terminal_blocked;
    diagnostic_out->layout_authority =
        boxrig_ui_view_is_layout_authority(widget->view);
    diagnostic_out->layout_configured = widget->allocated;
    diagnostic_out->metrics_revision = metrics.layout_revision;
    diagnostic_out->transparency_revision = transparency.revision;
    diagnostic_out->failed = input_diagnostic.failure_reason !=
                             BOXRIG_UI_COMPONENT_FAILURE_NONE;
    diagnostic_out->failure_reason = input_diagnostic.failure_reason;
    if (diagnostic_out->running &&
        (widget->animation_failure != BOXRIG_UI_OK ||
         boxrig_ui_view_animation_status(widget->view) != BOXRIG_UI_OK)) {
        diagnostic_out->running = false;
        diagnostic_out->failed = true;
        diagnostic_out->failure_reason = BOXRIG_UI_COMPONENT_FAILURE_ANIMATION;
    }
    diagnostic_out->native_input_queue_depth = input_diagnostic.queue_depth;
    diagnostic_out->native_input_queue_high_water =
        input_diagnostic.queue_high_water;
    diagnostic_out->native_input_queue_capacity =
        input_diagnostic.queue_capacity;
    diagnostic_out->active_clipboard_transfers =
        input_diagnostic.active_clipboard_transfers;
    diagnostic_out->outgoing_clipboard_bytes =
        input_diagnostic.outgoing_clipboard_bytes;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_frame_result *result_out)
{
    boxrig_terminal *terminal;
    cairo_surface_t *candidate;
    boxrig_ui_status status;
    bool needs_frame;
    bool input_blocked;
    bool input_dispatch;
    bool waiting_for_allocation;
    bool waiting_for_grid_resize;
    bool candidate_prepared;

    if (!accepts_calls(widget) || result_out == NULL ||
        (!widget->allocated && !widget->allocation_pending)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (widget->in_frame || widget->in_gl_render) {
        return BOXRIG_UI_BUSY;
    }
#if BOXRIG_GTK_OPENGL
    if (widget->gl_failed && widget->gl_retry_desktop) {
        /* Retry outside GTK's render signal so the failed area's resources
         * are released under their original context before replacing it. */
        release_gl_area(widget);
        widget->gl_failed = false;
        widget->gl_retry_desktop = false;
        widget->gl_desktop_only = true;
        widget->render_state.status = BOXRIG_UI_OK;
        widget->render_state.reason = BOXRIG_GTK_RENDER_REASON_REQUESTED;
        widget->render_state.gl_code = 0;
        widget->render_state.reset_status = 0;
        widget->render_state.message[0] = '\0';
        create_gl_area(widget);
    }
#endif
    if (widget->gl_failed && widget->render_state.requested_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED &&
        widget->render_state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE)
        return widget->render_state.status;
#if BOXRIG_GTK_OPENGL
    if (widget->gl_failed) release_gl_area(widget);
#endif
    terminal = boxrig_ui_model_terminal(widget->model);
    if (terminal == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = widget->animation_failure;
    if (status == BOXRIG_UI_OK) status = boxrig_ui_view_animation_status(widget->view);
    if (status == BOXRIG_UI_OK) status = boxrig_ui_model_synchronize(widget->model);
    if (status != BOXRIG_UI_OK) return status;
    status = boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, widget->scrollbar_visibility.state.requested);
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_BUSY) return status;
    widget->in_frame = true;
    if (widget->allocation_pending) {
        GtkAllocation allocation;

        gtk_widget_get_allocation(GTK_WIDGET(widget), &allocation);
        if (!prepare_widget_allocation(widget, &allocation)) {
            widget->in_frame = false;
            return BOXRIG_UI_UNAVAILABLE;
        }
    }
    retain_latest_grid_resize(widget);
    status = boxrig_gtk3_input_frame(
        widget->input,
        widget->maximum_frame_items,
        input_geometry_ready(widget),
        &input_blocked,
        &input_dispatch);
    if (status != BOXRIG_UI_OK) {
        set_accessibility_busy(widget, false);
        widget->in_frame = false;
        return status;
    }
    set_accessibility_busy(widget, input_blocked);
    retain_latest_grid_resize(widget);
    waiting_for_grid_resize = grid_resize_waiting(widget);
    if (!uses_gl(widget) && waiting_for_grid_resize && widget->presentation != NULL &&
        widget->presentation_allocation_generation != widget->allocation_generation) {
        /* Retry a failed extent copy without consuming model dirty. A zoom
         * candidate is not the backing preview for this actual allocation. */
        if (widget->surface_candidate_ready) {
            widget->in_frame = false;
            return BOXRIG_UI_UNAVAILABLE;
        }
        candidate = create_presentation_surface(
            widget, widget->output_width, widget->output_height);
        if (candidate == NULL) {
            widget->in_frame = false;
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        g_clear_pointer(&widget->presentation, cairo_surface_destroy);
        widget->presentation = candidate;
        widget->presentation_allocation_generation = widget->allocation_generation;
        if (widget->presentation_generation != UINT64_MAX) {
            widget->presentation_generation++;
        }
        gtk_widget_queue_draw(GTK_WIDGET(widget));
    }
    status = prepare_zoom_presentation_candidate(
        widget,
        &candidate_prepared);
    if (status != BOXRIG_UI_OK) {
        widget->in_frame = false;
        return status;
    }
    /* Native IME edits need a frame even while the terminal model is idle. */
    needs_frame = !has_presentation(widget) ||
                  widget->presentation_allocation_generation != widget->allocation_generation ||
                  boxrig_ui_view_frame_requested(widget->view) ||
                  current_composition_revision(widget) != widget->presentation_composition_revision;
    waiting_for_allocation = presentation_waits_for_allocation(widget);
    /* Keep the last complete image until native allocation and application
     * grid completion agree. The first image may use the current core grid so
     * an initial resize can reach its preview boundary without circular wait. */
    if (needs_frame && !waiting_for_allocation &&
        (!waiting_for_grid_resize || !has_presentation(widget) ||
         (uses_gl(widget) && widget->presentation_allocation_generation != widget->allocation_generation)) &&
        !boxrig_terminal_synchronized_output(terminal)) {
#if BOXRIG_GTK_OPENGL
        if (uses_gl(widget)) {
            widget->gl_pending = true;
            gtk_gl_area_queue_render(GTK_GL_AREA(widget->gl_area));
        } else
#endif
        {
            if (boxrig_pango_cairo_renderer_draw_view_composition(
                    widget->renderer,
                    widget->view,
                    boxrig_gtk3_input_composition(widget->input)) == -1) {
#if BOXRIG_GTK_OPENGL
                if (retain_previous_gl(widget, BOXRIG_UI_UNAVAILABLE, "Could not prepare the requested Cairo image"))
                    goto presentation_complete;
#endif
                widget->in_frame = false;
                return BOXRIG_UI_UNAVAILABLE;
            }
            candidate = create_presentation_surface(
                widget,
                widget->output_width,
                widget->output_height);
            if (candidate == NULL) {
#if BOXRIG_GTK_OPENGL
                if (retain_previous_gl(widget, BOXRIG_UI_OUT_OF_MEMORY, "Could not retain the requested Cairo image"))
                    goto presentation_complete;
#endif
                invalidate_complete_presentation(widget);
                widget->in_frame = false;
                return BOXRIG_UI_OUT_OF_MEMORY;
            }
            g_clear_pointer(&widget->presentation, cairo_surface_destroy);
            widget->presentation = candidate;
            widget->presentation_composition_revision = current_composition_revision(widget);
            widget->presentation_allocation_generation =
                widget->allocation_generation;
            if (widget->presentation_generation != UINT64_MAX) {
                widget->presentation_generation++;
            }
            gtk_widget_queue_draw(GTK_WIDGET(widget));
            if (widget->render_state.active_backend != BOXRIG_GTK_RENDER_BACKEND_CAIRO)
                widget->render_state.target_generation++;
            widget->render_state.active_backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
            widget->render_state.pending = false;
#if BOXRIG_GTK_OPENGL
            release_gl_area(widget);
#endif
        }
    }
#if BOXRIG_GTK_OPENGL
presentation_complete:
#endif
    if (input_geometry_ready(widget)) {
        boxrig_gtk3_input_update_candidate(widget->input, widget->output_scale);
    }
    synchronize_adjustments(widget);
    widget->in_frame = false;
    if (!waiting_for_allocation && !waiting_for_grid_resize &&
        !boxrig_ui_view_frame_requested(widget->view)) {
        status = complete_widget_zoom_generation(widget);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        waiting_for_allocation = presentation_waits_for_allocation(widget);
        retain_latest_grid_resize(widget);
        waiting_for_grid_resize = grid_resize_waiting(widget);
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->frame_pending =
        waiting_for_grid_resize ||
        boxrig_ui_view_frame_requested(widget->view);
    result_out->input_blocked = input_blocked;
    result_out->additional_dispatch =
        (!widget->gl_pending && !waiting_for_allocation && !waiting_for_grid_resize &&
         !boxrig_terminal_synchronized_output(terminal) && result_out->frame_pending) ||
        grid_resize_ready(widget) || input_dispatch || candidate_prepared ||
        surface_candidate_preparation_pending(widget);
    result_out->has_animation_deadline =
        boxrig_terminal_next_animation_deadline(
            terminal,
            &result_out->animation_deadline_milliseconds);
    uint64_t blink_deadline = 0;
    if (boxrig_ui_view_next_animation_deadline(widget->view, &blink_deadline) &&
        (!result_out->has_animation_deadline ||
         blink_deadline < result_out->animation_deadline_milliseconds)) {
        result_out->has_animation_deadline = true;
        result_out->animation_deadline_milliseconds = blink_deadline;
    }
    if (boxrig_terminal_synchronized_output(terminal)) return BOXRIG_UI_OK;
    /* Text signals run application and assistive-technology callbacks. Keep
     * the object alive and reject a nested frame even if a callback destroys
     * the widget while the old snapshot is being read for a removal. */
    g_object_ref(widget);
    widget->in_frame = true;
    status = boxrig_gtk3_accessible_update(widget);
    widget->in_frame = false;
    g_object_unref(widget);
    return status;
}

bool boxrig_gtk3_terminal_widget_has_presentation(
    const BoxrigGtk3TerminalWidget *widget)
{
    return widget != NULL && has_presentation(widget);
}

uint64_t boxrig_gtk3_terminal_widget_presentation_generation(
    const BoxrigGtk3TerminalWidget *widget)
{
    return widget == NULL ? 0 : widget->presentation_generation;
}
