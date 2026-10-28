/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/gtk4.h"
#include "boxrig/gtk4/terminal_widget.h"
#include "../../src/gtk4/input.h"
#include "../composition_fixture.h"
#include "../frontend_component_fixture.h"
#include "../../examples/gtk4/external_scrollbar.h"
#include "../external_scrollbar_example.h"

#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static GdkRectangle last_candidate_rectangle;
static size_t candidate_location_count;
static GtkIMContext *latest_im_context;

/** @copydoc gtk_im_multicontext_new */
GtkIMContext *__real_gtk_im_multicontext_new(void);

/** Observe context lifetime and signals independently of controller wiring. */
GtkIMContext *__wrap_gtk_im_multicontext_new(void)
{
    latest_im_context = __real_gtk_im_multicontext_new();
    return latest_im_context;
}

/** @copydoc gtk_im_context_set_cursor_location */
void __real_gtk_im_context_set_cursor_location(GtkIMContext *context,
    const GdkRectangle *area);

/** Records the candidate rectangle actually sent to the native input method. */
void __wrap_gtk_im_context_set_cursor_location(GtkIMContext *context,
    const GdkRectangle *area)
{
    last_candidate_rectangle = *area;
    candidate_location_count++;
    __real_gtk_im_context_set_cursor_location(context, area);
}

static bool current_event_state_overridden;
static GdkModifierType overridden_current_event_state;

GdkModifierType __real_gtk_event_controller_get_current_event_state(
    GtkEventController *controller);

GdkModifierType __wrap_gtk_event_controller_get_current_event_state(
    GtkEventController *controller)
{
    return current_event_state_overridden
               ? overridden_current_event_state
               : __real_gtk_event_controller_get_current_event_state(
                     controller);
}

static bool feed_bytes(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool take_content_size_action(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_size_command *command_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_gtk4_terminal_widget_take_layout_action(widget, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
        return false;
    }
    command_out->revision = action.revision;
    command_out->cause = action.value.content_size.cause;
    command_out->width = action.value.content_size.width;
    command_out->height = action.value.content_size.height;
    return true;
}

static bool snapshot_widget_bounds(
    GtkWidget *parent,
    GtkWidget *widget,
    graphene_rect_t *bounds_out)
{
    GtkSnapshot *snapshot = gtk_snapshot_new();
    GskRenderNode *node;

    CHECK(snapshot != NULL && bounds_out != NULL);
    gtk_widget_snapshot_child(parent, widget, snapshot);
    node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    gsk_render_node_get_bounds(node, bounds_out);
    gsk_render_node_unref(node);
    return true;
}

static bool snapshot_widget(GtkWidget *parent, GtkWidget *widget)
{
    graphene_rect_t bounds;

    return snapshot_widget_bounds(parent, widget, &bounds);
}

static bool snapshot_widget_pixel(
    GtkWidget *parent,
    GtkWidget *widget,
    int x,
    int y,
    uint32_t *pixel_out)
{
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    cairo_surface_t *surface;
    cairo_t *cairo;
    unsigned char *pixels;
    int stride;

    CHECK(width > 0 && height > 0 && x >= 0 && x < width && y >= 0 &&
          y < height && pixel_out != NULL);
    snapshot = gtk_snapshot_new();
    CHECK(snapshot != NULL);
    gtk_widget_snapshot_child(parent, widget, snapshot);
    node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    CHECK(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    cairo = cairo_create(surface);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    gsk_render_node_draw(node, cairo);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cairo);
    gsk_render_node_unref(node);
    cairo_surface_flush(surface);
    pixels = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    CHECK(pixels != NULL && stride >= width * 4);
    memcpy(pixel_out, pixels + (size_t)y * (size_t)stride + (size_t)x * 4,
           sizeof(*pixel_out));
    cairo_surface_destroy(surface);
    return true;
}

static void mark_widget_mapped(GtkWidget *widget, gpointer user_data)
{
    bool *mapped = user_data;

    (void)widget;
    *mapped = true;
}

static void count_frame_request(void *context)
{
    size_t *count = context;

    (*count)++;
}

static gboolean expire_layout_boundary_wait(gpointer context)
{
    bool *expired = context;

    *expired = true;
    return G_SOURCE_REMOVE;
}

typedef struct key_route_observer {
    size_t count;
    boxrig_key_action actions[8];
    boxrig_key_event events[8];
} key_route_observer;

typedef struct mouse_route_observer {
    size_t count;
    boxrig_mouse_event events[8];
    boxrig_ui_hit_test_result hits[8];
} mouse_route_observer;

typedef struct clipboard_read_result {
    char *text;
    GError *error;
    bool completed;
} clipboard_read_result;

typedef struct gtk4_fixture_host {
    GtkWindow *window;
    BoxrigGtk4TerminalWidget *attached_widget;
} gtk4_fixture_host;

typedef struct gtk4_fixture_component {
    gtk4_fixture_host *host;
    BoxrigGtk4TerminalWidget *widget;
} gtk4_fixture_component;

static bool gtk4_fixture_run_native_input_transcript(
    void *opaque_component,
    const boxrig_test_frontend_input_script *script,
    boxrig_test_frontend_input_observation *observation_out);

static boxrig_input_key_route capture_key_series(
    const boxrig_key_event *event,
    void *context)
{
    key_route_observer *observer = context;

    if (observer->count < sizeof(observer->actions) /
                              sizeof(observer->actions[0])) {
        observer->actions[observer->count] = event->action;
        observer->events[observer->count] = *event;
    }
    observer->count++;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static boxrig_input_mouse_route capture_left_drag(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    mouse_route_observer *observer = context;

    if (hit != NULL &&
        observer->count < sizeof(observer->events) /
                              sizeof(observer->events[0])) {
        observer->events[observer->count] = *event;
        observer->hits[observer->count] = *hit;
    }
    observer->count++;
    return event->action == BOXRIG_MOUSE_PRESS &&
                   event->button == BOXRIG_MOUSE_BUTTON_LEFT
               ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
               : BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static void clipboard_text_read(
    GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
    clipboard_read_result *read_result = user_data;

    read_result->text = gdk_clipboard_read_text_finish(
        GDK_CLIPBOARD(source),
        result,
        &read_result->error);
    read_result->completed = true;
}

static void *gtk4_fixture_create_host(void)
{
    gtk4_fixture_host *host = calloc(1, sizeof(*host));

    if (host == NULL) {
        return NULL;
    }
    host->window = GTK_WINDOW(gtk_window_new());
    if (host->window == NULL) {
        free(host);
        return NULL;
    }
    g_object_ref_sink(host->window);
    gtk_window_set_decorated(host->window, false);
    return host;
}

static void gtk4_fixture_destroy_host(void *opaque_host)
{
    gtk4_fixture_host *host = opaque_host;

    if (host == NULL) {
        return;
    }
    gtk_window_destroy(host->window);
    g_object_unref(host->window);
    free(host);
}

static boxrig_ui_status gtk4_fixture_create_component(
    void *opaque_host,
    boxrig_ui_model *model,
    boxrig_ui_component_attachment attachment,
    void **component_out)
{
    gtk4_fixture_host *host = opaque_host;
    boxrig_gtk4_terminal_widget_config config =
        boxrig_gtk4_terminal_widget_config_default();
    gtk4_fixture_component *component;
    BoxrigGtk4TerminalWidget *widget = NULL;
    boxrig_ui_status status;

    *component_out = NULL;
    component = calloc(1, sizeof(*component));
    if (component == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    config.attachment = attachment;
    if (getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL)
        config.render_policy = BOXRIG_GTK_RENDER_OPENGL_REQUIRED;
    config.paste_chunk_size = 4;
    status = boxrig_gtk4_terminal_widget_create(model, &config, &widget);
    if (status != BOXRIG_UI_OK) {
        free(component);
        return status;
    }
    g_object_ref_sink(widget);
    component->host = host;
    component->widget = widget;
    gtk_widget_set_margin_start(GTK_WIDGET(widget), 31);
    gtk_widget_set_margin_top(GTK_WIDGET(widget), 17);
    if (host->attached_widget == NULL) {
        gtk_window_set_child(host->window, GTK_WIDGET(widget));
        host->attached_widget = widget;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

static void gtk4_fixture_destroy_component(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    if (component == NULL) {
        return;
    }
    (void)boxrig_gtk4_terminal_widget_quiesce(component->widget);
    if (component->host->attached_widget == component->widget) {
        gtk_window_set_child(component->host->window, NULL);
        component->host->attached_widget = NULL;
    }
    g_object_unref(component->widget);
    free(component);
}

static boxrig_ui_status gtk4_fixture_quiesce_component(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_quiesce(component->widget);
}

static boxrig_ui_view *gtk4_fixture_component_view(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_view(component->widget);
}

static bool gtk4_fixture_get_effective_metrics(
    void *opaque_component,
    boxrig_font_effective_metrics *metrics_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_effective_metrics(
               component->widget,
               metrics_out) == BOXRIG_FONT_CATALOG_OK;
}

static boxrig_ui_status gtk4_fixture_allocate(
    void *opaque_component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    gtk4_fixture_component *component = opaque_component;
    GtkWidget *widget = GTK_WIDGET(component->widget);
    boxrig_ui_status status;
    int scale = gtk_widget_get_scale_factor(widget);
    unsigned int logical_width;
    unsigned int logical_height;

    if (scale <= 0 || allocation->width > (unsigned int)INT_MAX ||
        allocation->height > (unsigned int)INT_MAX) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    logical_width =
        (allocation->width + (unsigned int)scale - 1) / (unsigned int)scale;
    logical_height =
        (allocation->height + (unsigned int)scale - 1) / (unsigned int)scale;
    status = boxrig_gtk4_terminal_widget_set_window_state(
        component->widget,
        allocation->window_state);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    gtk_widget_set_size_request(
        widget,
        (int)logical_width,
        (int)logical_height);
    gtk_window_set_default_size(
        component->host->window,
        (int)logical_width + 31,
        (int)logical_height + 17);
    gtk_window_present(component->host->window);
    while (boxrig_gtk4_terminal_widget_get_layout(
               component->widget,
               layout_out) == BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status gtk4_fixture_set_preferred_grid(
    void *opaque_component,
    size_t rows,
    size_t columns)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_set_preferred_grid(
        component->widget,
        rows,
        columns);
}

static bool gtk4_fixture_get_preferred_grid(
    void *opaque_component,
    size_t *rows_out,
    size_t *columns_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_preferred_grid(
               component->widget,
               rows_out,
               columns_out) == BOXRIG_UI_OK;
}

static boxrig_ui_size_command_state gtk4_fixture_get_size_command_state(
    void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;
    boxrig_ui_size_command_state state = BOXRIG_UI_SIZE_COMMAND_IDLE;

    (void)boxrig_gtk4_terminal_widget_get_size_command_state(
        component->widget,
        &state);
    return state;
}

static bool gtk4_fixture_take_layout_action(
    void *opaque_component,
    boxrig_ui_layout_action *action_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_take_layout_action(
        component->widget,
        action_out);
}

static boxrig_ui_status gtk4_fixture_complete_size_command(
    void *opaque_component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_complete_size_command(
        component->widget,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
}

static boxrig_ui_status gtk4_fixture_complete_grid_resize(
    void *opaque_component,
    uint64_t revision,
    bool succeeded)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_complete_grid_resize(
        component->widget, revision, succeeded);
}

static boxrig_ui_status gtk4_fixture_set_visible(
    void *opaque_component,
    bool visible)
{
    gtk4_fixture_component *component = opaque_component;
    boxrig_ui_status status = boxrig_gtk4_terminal_widget_set_visible(
        component->widget,
        visible);
    size_t event_count;

    if (status == BOXRIG_UI_OK) {
        gtk_widget_set_visible(GTK_WIDGET(component->widget), visible);
        if (visible) {
            gtk_window_present(component->host->window);
            for (event_count = 0;
                 event_count < 128 &&
                 (gtk_widget_get_width(GTK_WIDGET(component->widget)) == 0 ||
                  gtk_widget_get_height(GTK_WIDGET(component->widget)) == 0);
                 event_count++) {
                (void)g_main_context_iteration(NULL, true);
            }
            if (gtk_widget_get_width(GTK_WIDGET(component->widget)) == 0 ||
                gtk_widget_get_height(GTK_WIDGET(component->widget)) == 0) {
                return BOXRIG_UI_UNAVAILABLE;
            }
        }
    }
    return status;
}

static boxrig_ui_status gtk4_fixture_set_frame_request_callback(
    void *opaque_component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_set_frame_request_callback(
        component->widget,
        callback,
        context);
}

static boxrig_ui_status gtk4_fixture_frame(
    void *opaque_component,
    boxrig_ui_component_frame_result *result_out)
{
    gtk4_fixture_component *component = opaque_component;

    boxrig_ui_status status = boxrig_gtk4_terminal_widget_frame(component->widget, result_out);
    if (status == BOXRIG_UI_OK && getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL) {
        /* Complete the queued toolkit snapshot for the shared fixture's
         * synchronous observation of the accepted frame. */
        if (!snapshot_widget(GTK_WIDGET(component->host->window), GTK_WIDGET(component->widget)))
            return BOXRIG_UI_UNAVAILABLE;
        status = boxrig_gtk4_terminal_widget_frame(component->widget, result_out);
        boxrig_gtk_render_state state;
        if (status == BOXRIG_UI_OK &&
            (boxrig_gtk4_terminal_widget_get_render_state(component->widget, &state) != BOXRIG_UI_OK ||
             state.active_backend != BOXRIG_GTK_RENDER_BACKEND_OPENGL)) return BOXRIG_UI_UNAVAILABLE;
    }
    return status;
}

static boxrig_ui_status gtk4_fixture_queue_paste(
    void *opaque_component,
    const uint8_t *bytes,
    size_t length)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_queue_paste(
        component->widget,
        bytes,
        length);
}

static boxrig_font_catalog_status gtk4_fixture_register_font_file(
    void *opaque_component,
    const char *path,
    size_t path_length,
    boxrig_font_resource_id *resource_id_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_register_font_file(
        component->widget,
        path,
        path_length,
        0,
        resource_id_out);
}

static boxrig_ui_status gtk4_fixture_set_font_specification(
    void *opaque_component,
    const boxrig_font_specification *specification)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_set_font_specification(
        component->widget,
        specification);
}

static boxrig_ui_status gtk4_fixture_get_font_specification(
    void *opaque_component,
    boxrig_font_specification *specification_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_font_specification(
        component->widget,
        specification_out);
}

static boxrig_font_resource_id gtk4_fixture_primary_font_resource(
    void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_primary_font_resource(
        component->widget);
}

static uint64_t gtk4_fixture_font_catalog_revision(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_font_catalog_revision(
        component->widget);
}

static boxrig_ui_status gtk4_fixture_set_line_height(
    void *opaque_component,
    float adjustment_dip)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_set_line_height_adjustment(
        component->widget,
        adjustment_dip);
}

static boxrig_ui_status gtk4_fixture_get_line_height(
    void *opaque_component,
    float *adjustment_dip_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_line_height_adjustment(
        component->widget,
        adjustment_dip_out);
}

static boxrig_ui_status gtk4_fixture_visual_zoom(
    void *opaque_component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_visual_zoom(
        component->widget,
        direction,
        policy,
        changed_out);
}

static boxrig_ui_status gtk4_fixture_visual_zoom_reset(
    void *opaque_component,
    bool *changed_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_visual_zoom_reset(
        component->widget,
        changed_out);
}

static boxrig_ui_status gtk4_fixture_set_text_direction(
    void *opaque_component,
    boxrig_text_direction_policy policy)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_set_text_direction(
        component->widget,
        policy);
}

static boxrig_text_direction_policy gtk4_fixture_text_direction(
    void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_text_direction(component->widget);
}

static bool gtk4_fixture_copy_selection(
    void *opaque_host,
    void *opaque_component,
    const uint8_t *expected,
    size_t expected_length)
{
    gtk4_fixture_component *component = opaque_component;
    boxrig_ui_view_action action;
    clipboard_read_result read_result;
    GdkClipboard *clipboard;
    bool matched;

    (void)opaque_host;
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    if (boxrig_gtk4_terminal_widget_apply_view_action(
            component->widget,
            &action) != BOXRIG_UI_OK) {
        return false;
    }
    clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(component->widget)));
    memset(&read_result, 0, sizeof(read_result));
    gdk_clipboard_read_text_async(
        clipboard,
        NULL,
        clipboard_text_read,
        &read_result);
    while (!read_result.completed) {
        (void)g_main_context_iteration(NULL, true);
    }
    matched = read_result.error == NULL && read_result.text != NULL &&
              strlen(read_result.text) == expected_length &&
              memcmp(read_result.text, expected, expected_length) == 0;
    g_clear_error(&read_result.error);
    g_free(read_result.text);
    return matched;
}

static boxrig_ui_status gtk4_fixture_set_transparency(
    void *opaque_component,
    const boxrig_ui_transparency_config *config)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_set_transparency(
        component->widget,
        config);
}

static boxrig_ui_status gtk4_fixture_get_transparency(
    void *opaque_component,
    boxrig_ui_transparency_state *state_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_transparency(
        component->widget,
        state_out);
}

static uint32_t gtk4_fixture_capabilities(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_capabilities(component->widget);
}

static boxrig_ui_status gtk4_fixture_get_diagnostic(
    void *opaque_component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_diagnostic(
        component->widget,
        diagnostic_out);
}

static boxrig_composition *gtk4_fixture_composition(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_composition(component->widget);
}

static bool gtk4_fixture_observe_presentation(
    void *opaque_component,
    const boxrig_test_frontend_presentation_probe *probe,
    boxrig_test_frontend_presentation_observation *observation_out)
{
    gtk4_fixture_component *component = opaque_component;
    boxrig_test_frontend_presentation_probe logical_probe;
    GtkWidget *widget;
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    cairo_surface_t *surface;
    cairo_t *cairo;
    unsigned char *data;
    uint32_t background;
    uint32_t meaning;
    uint64_t stable_hash = UINT64_C(1469598103934665603);
    size_t coverage = 0;
    int stride;
    int width;
    int height;
    int scale;
    unsigned int x;
    unsigned int y;

    if (component == NULL || probe == NULL || observation_out == NULL) {
        return false;
    }
    widget = GTK_WIDGET(component->widget);
    scale = gtk_widget_get_scale_factor(widget);
    if (scale <= 0) {
        return false;
    }
    {
        boxrig_composition_view composition;
        CHECK(boxrig_composition_get_view(
            boxrig_gtk4_terminal_widget_composition(component->widget),
            &composition) == BOXRIG_COMPOSITION_OK);
        if (composition.candidate.visible) {
            CHECK(candidate_location_count != 0);
            CHECK(last_candidate_rectangle.x == (int)(composition.candidate.spot_x / scale));
            CHECK(last_candidate_rectangle.y == (int)(composition.candidate.exclude_y / scale));
            CHECK(last_candidate_rectangle.width ==
                (int)((composition.candidate.exclude_width + scale - 1) / scale));
            CHECK(last_candidate_rectangle.height ==
                (int)((composition.candidate.exclude_height + scale - 1) / scale));
        }
    }
    logical_probe = *probe;
    logical_probe.background_x /= scale;
    logical_probe.background_y /= scale;
    logical_probe.meaning_x /= scale;
    logical_probe.meaning_y /= scale;
    logical_probe.coverage_x /= scale;
    logical_probe.coverage_y /= scale;
    logical_probe.coverage_width =
        (logical_probe.coverage_width + (unsigned int)scale - 1) /
        (unsigned int)scale;
    logical_probe.coverage_height =
        (logical_probe.coverage_height + (unsigned int)scale - 1) /
        (unsigned int)scale;
    logical_probe.stable_x /= scale;
    logical_probe.stable_y /= scale;
    logical_probe.stable_width =
        (logical_probe.stable_width + (unsigned int)scale - 1) /
        (unsigned int)scale;
    logical_probe.stable_height =
        (logical_probe.stable_height + (unsigned int)scale - 1) /
        (unsigned int)scale;
    probe = &logical_probe;
    width = gtk_widget_get_width(widget);
    height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0 || probe->background_x < 0 ||
        probe->background_y < 0 || probe->meaning_x < 0 ||
        probe->meaning_y < 0 || probe->coverage_x < 0 ||
        probe->coverage_y < 0 || probe->stable_x < 0 ||
        probe->stable_y < 0 || probe->background_x >= width ||
        probe->background_y >= height || probe->meaning_x >= width ||
        probe->meaning_y >= height || probe->coverage_x >= width ||
        probe->coverage_y >= height || probe->stable_x >= width ||
        probe->stable_y >= height || probe->coverage_width == 0 ||
        probe->coverage_height == 0 || probe->stable_width == 0 ||
        probe->stable_height == 0 ||
        probe->coverage_width > (unsigned int)(width - probe->coverage_x) ||
        probe->coverage_height >
            (unsigned int)(height - probe->coverage_y) ||
        probe->stable_width > (unsigned int)(width - probe->stable_x) ||
        probe->stable_height > (unsigned int)(height - probe->stable_y)) {
        fprintf(
            stderr,
            "GTK 4 presentation probe outside %dx%d: background=%d,%d "
            "meaning=%d,%d coverage=%d,%d+%ux%u stable=%d,%d+%ux%u\n",
            width,
            height,
            probe->background_x,
            probe->background_y,
            probe->meaning_x,
            probe->meaning_y,
            probe->coverage_x,
            probe->coverage_y,
            probe->coverage_width,
            probe->coverage_height,
            probe->stable_x,
            probe->stable_y,
            probe->stable_width,
            probe->stable_height);
        return false;
    }
    snapshot = gtk_snapshot_new();
    if (snapshot == NULL) {
        return false;
    }
    /* snapshot_child uses window coordinates, while probes are widget-local. */
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(-31, -17));
    gtk_widget_snapshot_child(
        GTK_WIDGET(component->host->window),
        widget,
        snapshot);
    node = gtk_snapshot_free_to_node(snapshot);
    if (node == NULL) {
        fprintf(stderr, "GTK 4 presentation snapshot produced no node\n");
        return false;
    }
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        gsk_render_node_unref(node);
        return false;
    }
    cairo = cairo_create(surface);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cairo);
        cairo_surface_destroy(surface);
        gsk_render_node_unref(node);
        return false;
    }
    gsk_render_node_draw(node, cairo);
    gsk_render_node_unref(node);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cairo);
        cairo_surface_destroy(surface);
        return false;
    }
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    data = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    if (data == NULL || stride < width * 4) {
        cairo_surface_destroy(surface);
        return false;
    }
    memcpy(
        &background,
        data + (size_t)probe->background_y * (size_t)stride +
            (size_t)probe->background_x * sizeof(background),
        sizeof(background));
    memcpy(
        &meaning,
        data + (size_t)probe->meaning_y * (size_t)stride +
            (size_t)probe->meaning_x * sizeof(meaning),
        sizeof(meaning));
    for (y = 0; y < probe->coverage_height; y++) {
        for (x = 0; x < probe->coverage_width; x++) {
            uint32_t pixel;

            memcpy(
                &pixel,
                data + (size_t)(probe->coverage_y + (int)y) *
                           (size_t)stride +
                    (size_t)(probe->coverage_x + (int)x) * sizeof(pixel),
                sizeof(pixel));
            if (pixel != background) {
                coverage++;
            }
        }
    }
    for (y = 0; y < probe->stable_height; y++) {
        for (x = 0; x < probe->stable_width; x++) {
            uint32_t pixel;

            memcpy(
                &pixel,
                data + (size_t)(probe->stable_y + (int)y) *
                           (size_t)stride +
                    (size_t)(probe->stable_x + (int)x) * sizeof(pixel),
                sizeof(pixel));
            stable_hash ^= pixel;
            stable_hash *= UINT64_C(1099511628211);
        }
    }
    cairo_surface_destroy(surface);
    memset(observation_out, 0, sizeof(*observation_out));
    observation_out->generation =
        boxrig_gtk4_terminal_widget_presentation_generation(
            component->widget);
    observation_out->width = (unsigned int)width * (unsigned int)scale;
    observation_out->height = (unsigned int)height * (unsigned int)scale;
    observation_out->background_argb = background;
    observation_out->meaning_argb = meaning;
    observation_out->coverage_pixels = coverage;
    observation_out->stable_hash = stable_hash;
    observation_out->alpha_observable = true;
    return true;
}

static boxrig_ui_view_action_capabilities
gtk4_fixture_view_action_capabilities(void *opaque_component)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_view_action_capabilities(
        component->widget);
}

static boxrig_ui_status gtk4_fixture_apply_view_action(
    void *opaque_component,
    const boxrig_ui_view_action *action)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_apply_view_action(
        component->widget,
        action);
}

static bool gtk4_fixture_get_scrollbar_state(void *component, boxrig_scrollbar_state *state_out)
{
    gtk4_fixture_component *fixture = component;
    example_external_scrollbar bar = {.component = fixture->widget};
    return check_external_scrollbar_example(&bar, state_out);
}

static bool gtk4_fixture_get_scrollbar_model(
    void *opaque_component,
    boxrig_scrollbar_model *model_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_get_scrollbar_model(
               component->widget,
               model_out) == BOXRIG_UI_OK;
}

static boxrig_ui_status gtk4_fixture_apply_scrollbar_action(
    void *opaque_component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    gtk4_fixture_component *component = opaque_component;

    return boxrig_gtk4_terminal_widget_apply_scrollbar_action(
        component->widget,
        action,
        position,
        changed_out);
}

static bool test_widget_frontend_component_fixture(const char *font_path)
{
    const boxrig_test_frontend_component_operations operations = {
        .frontend_name = "GTK 4",
        .lifecycle = {
            .create_host = gtk4_fixture_create_host,
            .destroy_host = gtk4_fixture_destroy_host,
            .create_component = gtk4_fixture_create_component,
            .quiesce_component = gtk4_fixture_quiesce_component,
            .destroy_component = gtk4_fixture_destroy_component},
        .layout = {
            .component_view = gtk4_fixture_component_view,
            .get_effective_metrics = gtk4_fixture_get_effective_metrics,
            .allocate = gtk4_fixture_allocate,
            .set_preferred_grid = gtk4_fixture_set_preferred_grid,
            .get_preferred_grid = gtk4_fixture_get_preferred_grid,
            .get_size_command_state =
                gtk4_fixture_get_size_command_state,
            .take_layout_action = gtk4_fixture_take_layout_action,
            .complete_size_command =
                gtk4_fixture_complete_size_command,
            .complete_grid_resize =
                gtk4_fixture_complete_grid_resize,
            .set_visible = gtk4_fixture_set_visible},
        .frame = {
            .set_frame_request_callback =
                gtk4_fixture_set_frame_request_callback,
            .frame = gtk4_fixture_frame},
        .input = {
            .queue_paste = gtk4_fixture_queue_paste,
            .run_native_transcript =
                gtk4_fixture_run_native_input_transcript},
        .font = {
            .fixture_font_path = font_path,
            .register_font_file = gtk4_fixture_register_font_file,
            .set_specification = gtk4_fixture_set_font_specification,
            .get_specification = gtk4_fixture_get_font_specification,
            .primary_resource = gtk4_fixture_primary_font_resource,
            .catalog_revision = gtk4_fixture_font_catalog_revision,
            .set_line_height = gtk4_fixture_set_line_height,
            .get_line_height = gtk4_fixture_get_line_height,
            .visual_zoom = gtk4_fixture_visual_zoom,
            .visual_zoom_reset = gtk4_fixture_visual_zoom_reset,
            .set_text_direction = gtk4_fixture_set_text_direction,
            .text_direction = gtk4_fixture_text_direction},
        .view_action = {
            .copy_selection = gtk4_fixture_copy_selection,
            .capabilities = gtk4_fixture_view_action_capabilities,
            .apply = gtk4_fixture_apply_view_action,
            .get_scrollbar_model = gtk4_fixture_get_scrollbar_model,
            .get_scrollbar_state = gtk4_fixture_get_scrollbar_state,
            .apply_scrollbar_action =
                gtk4_fixture_apply_scrollbar_action},
        .presentation = {
            .set_transparency = gtk4_fixture_set_transparency,
            .get_transparency = gtk4_fixture_get_transparency,
            .capabilities = gtk4_fixture_capabilities,
            .get_diagnostic = gtk4_fixture_get_diagnostic,
            .composition = gtk4_fixture_composition,
            .observe = gtk4_fixture_observe_presentation}};

    return boxrig_test_run_frontend_component_fixture(&operations);
}

static GObject *find_widget_controller(GtkWidget *widget, GType type)
{
    GListModel *controllers = gtk_widget_observe_controllers(widget);
    GObject *match = NULL;
    guint count;
    guint index;

    if (controllers == NULL) {
        return NULL;
    }
    count = g_list_model_get_n_items(controllers);
    for (index = 0; index < count; index++) {
        GObject *candidate = g_list_model_get_item(controllers, index);

        if (candidate != NULL && g_type_is_a(G_OBJECT_TYPE(candidate), type)) {
            match = candidate;
            break;
        }
        g_clear_object(&candidate);
    }
    g_object_unref(controllers);
    return match;
}

static bool weak_reference_is_empty(GWeakRef *reference)
{
    GObject *object = g_weak_ref_get(reference);

    if (object == NULL) {
        return true;
    }
    g_object_unref(object);
    return false;
}

static bool emit_key_press(
    GtkEventControllerKey *controller,
    guint keyval,
    guint keycode,
    GdkModifierType state)
{
    gboolean handled = FALSE;

    g_signal_emit_by_name(
        controller,
        "key-pressed",
        keyval,
        keycode,
        state,
        &handled);
    return handled != FALSE;
}

static void emit_key_release(
    GtkEventControllerKey *controller,
    guint keyval,
    guint keycode,
    GdkModifierType state)
{
    g_signal_emit_by_name(
        controller,
        "key-released",
        keyval,
        keycode,
        state);
}

static bool gtk4_fixture_run_native_input_transcript(
    void *opaque_component,
    const boxrig_test_frontend_input_script *script,
    boxrig_test_frontend_input_observation *observation_out)
{
    gtk4_fixture_component *component = opaque_component;
    GtkWidget *widget;
    GtkEventControllerKey *key_controller = NULL;
    GtkGestureClick *click_gesture = NULL;
    GtkEventControllerMotion *motion_controller = NULL;
    boxrig_ui_component_frame_result frame_result;
    unsigned int scale;
    double x;
    double y;
    bool passed = false;

    if (component == NULL || script == NULL || observation_out == NULL) {
        return false;
    }
    widget = GTK_WIDGET(component->widget);
    scale = (unsigned int)gtk_widget_get_scale_factor(widget);
    if (scale == 0 ||
        boxrig_gtk4_terminal_widget_set_key_router(
            component->widget,
            boxrig_test_frontend_capture_key,
            observation_out) != BOXRIG_UI_OK ||
        boxrig_gtk4_terminal_widget_set_mouse_router(
            component->widget,
            boxrig_test_frontend_capture_mouse,
            observation_out) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    key_controller = GTK_EVENT_CONTROLLER_KEY(find_widget_controller(
        widget,
        GTK_TYPE_EVENT_CONTROLLER_KEY));
    click_gesture = GTK_GESTURE_CLICK(find_widget_controller(
        widget,
        GTK_TYPE_GESTURE_CLICK));
    motion_controller = GTK_EVENT_CONTROLLER_MOTION(find_widget_controller(
        widget,
        GTK_TYPE_EVENT_CONTROLLER_MOTION));
    if (key_controller == NULL || click_gesture == NULL ||
        motion_controller == NULL) {
        goto cleanup;
    }
    x = (double)script->x /
        ((double)BOXRIG_UI_FIXED_ONE * (double)scale);
    y = (double)script->y /
        ((double)BOXRIG_UI_FIXED_ONE * (double)scale);

    gtk_window_set_focus(component->host->window, widget);
    while (g_main_context_iteration(NULL, false)) {
    }
    observation_out->focused_after_enter =
        gtk_window_get_focus(component->host->window) == widget;
    if (!emit_key_press(key_controller, GDK_KEY_F1, 67, 0) ||
        !emit_key_press(key_controller, GDK_KEY_F1, 67, 0)) {
        goto cleanup;
    }
    emit_key_release(key_controller, GDK_KEY_F1, 67, 0);

    current_event_state_overridden = true;
    overridden_current_event_state = GDK_CONTROL_MASK;
    g_signal_emit_by_name(click_gesture, "pressed", 1, x, y);
    overridden_current_event_state = GDK_SHIFT_MASK;
    g_signal_emit_by_name(motion_controller, "motion", x, y);
    overridden_current_event_state = 0;
    g_signal_emit_by_name(click_gesture, "released", 1, x, y);
    current_event_state_overridden = false;
    if (boxrig_gtk4_terminal_widget_frame(
            component->widget,
            &frame_result) != BOXRIG_UI_OK) {
        goto cleanup;
    }

    gtk_window_set_focus(component->host->window, NULL);
    while (g_main_context_iteration(NULL, false)) {
    }
    observation_out->focused_after_leave =
        gtk_window_get_focus(component->host->window) != NULL;
    passed = true;

cleanup:
    current_event_state_overridden = false;
    g_clear_object(&key_controller);
    g_clear_object(&click_gesture);
    g_clear_object(&motion_controller);
    if (boxrig_gtk4_terminal_widget_set_key_router(
            component->widget,
            NULL,
            NULL) != BOXRIG_UI_OK ||
        boxrig_gtk4_terminal_widget_set_mouse_router(
            component->widget,
            NULL,
            NULL) != BOXRIG_UI_OK) {
        passed = false;
    }
    return passed;
}

static uint8_t *read_font_file(const char *path, size_t *length_out)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (data == NULL ||
        fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length_out = (size_t)length;
    return data;
}

static void add_preedit_attribute(
    PangoAttrList *attributes,
    PangoAttribute *attribute,
    guint start,
    guint end)
{
    attribute->start_index = start;
    attribute->end_index = end;
    pango_attr_list_insert(attributes, attribute);
}

static bool test_widget_preedit_translation(void)
{
    boxrig_gtk4_preedit_translation translation;
    PangoAttrList *attributes;

    memset(&translation, 0, sizeof(translation));
    attributes = pango_attr_list_new();
    CHECK(attributes != NULL);
    add_preedit_attribute(
        attributes,
        pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
        0,
        1);
    add_preedit_attribute(
        attributes,
        pango_attr_weight_new(PANGO_WEIGHT_BOLD),
        1,
        4);
    add_preedit_attribute(
        attributes,
        pango_attr_background_new(0, 0, 0),
        4,
        7);
    CHECK(boxrig_gtk4_input_translate_preedit(
        boxrig_composition_fixture.utf8,
        attributes,
        2,
        &translation));
    CHECK(translation.segment_count ==
          boxrig_composition_fixture.segment_count);
    CHECK(memcmp(
              translation.segments,
              boxrig_composition_fixture.segments,
              translation.segment_count * sizeof(*translation.segments)) ==
          0);
    CHECK(translation.selection_offset ==
          boxrig_composition_fixture.selection_offset);
    CHECK(translation.selection_length ==
          boxrig_composition_fixture.selection_length);
    CHECK(translation.caret_offset ==
          boxrig_composition_fixture.caret_offset);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    attributes = pango_attr_list_new();
    CHECK(attributes != NULL);
    add_preedit_attribute(
        attributes,
        pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
        4,
        5);
    CHECK(boxrig_gtk4_input_translate_preedit(
        boxrig_composition_fixture.utf8,
        attributes,
        2,
        &translation));
    /* The end of ASCII e is a valid scalar boundary inside e + combining acute. */
    CHECK(translation.segment_count == 1);
    CHECK(translation.segments[0].text_offset == 4 && translation.segments[0].text_length == 1);
    CHECK(translation.segments[0].feedback == BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);
    return true;
}

static bool test_widget_lifecycle_layout_and_frame(const char *font_path)
{
    static const uint8_t first_text[] = "boxrig";
    static const uint8_t second_text[] = "!";
    static const uint8_t remapped_text[] = "?";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    BoxrigGtk4TerminalWidget *conflict = NULL;
    BoxrigGtk4TerminalWidget *presentation = NULL;
    BoxrigGtk4TerminalWidget *replacement = NULL;
    GtkWindow *window;
    boxrig_font_effective_metrics metrics;
    boxrig_font_effective_metrics changed_metrics;
    boxrig_font_effective_metrics preserved_metrics;
    boxrig_font_specification font_specification;
    boxrig_font_specification reported_specification;
    boxrig_font_family resource_family;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_rendering_policy reported_policy;
    boxrig_font_effective_metrics resource_metrics;
    boxrig_font_effective_metrics policy_metrics;
    boxrig_font_effective_metrics line_height_metrics;
    boxrig_font_effective_metrics zoomed_metrics;
    boxrig_font_effective_metrics reset_metrics;
    boxrig_font_visual_step_policy zoom_policy;
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action layout_action;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_ui_dirty dirty;
    GtkAdjustment *horizontal;
    GtkAdjustment *vertical;
    int minimum_width;
    int natural_width;
    int minimum_height;
    int natural_height;
    int scale;
    int width;
    int height;
    uint8_t *font_data = NULL;
    size_t font_length = 0;
    size_t frame_request_count = 0;
    size_t requests_before_unmap;
    boxrig_font_resource_id file_resource = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id memory_resource = BOXRIG_FONT_RESOURCE_NONE;
    uint64_t catalog_revision;
    uint64_t first_generation;
    guint layout_timeout;
    float line_height = 0.0f;
    bool layout_expired = false;
    bool zoom_changed = false;
    bool mapped = false;

    CHECK(widget_config.maximum_native_input_events == 256);
    CHECK(widget_config.maximum_frame_items == 256);
    terminal_config.rows = 3;
    terminal_config.columns = 8;
    terminal_config.scrollback_capacity = 32;
    widget_config.frame_request_callback = count_frame_request;
    widget_config.frame_request_context = &frame_request_count;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    CHECK(widget != NULL);
    g_object_ref_sink(widget);
    CHECK(GTK_IS_WIDGET(widget));
    CHECK(GTK_IS_SCROLLABLE(widget));
    CHECK(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(widget)) ==
          GTK_ACCESSIBLE_ROLE_TERMINAL);
    CHECK(boxrig_gtk4_terminal_widget_model(widget) == model);
    CHECK(boxrig_ui_view_model(
              boxrig_gtk4_terminal_widget_view(widget)) == model);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &conflict) == BOXRIG_UI_UNAVAILABLE);
    CHECK(conflict == NULL);
    widget_config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &presentation) == BOXRIG_UI_OK);
    g_object_ref_sink(presentation);
    g_object_unref(presentation);
    presentation = NULL;

    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale > 0);
    gtk_widget_measure(
        GTK_WIDGET(widget),
        GTK_ORIENTATION_HORIZONTAL,
        -1,
        &minimum_width,
        &natural_width,
        NULL,
        NULL);
    gtk_widget_measure(
        GTK_WIDGET(widget),
        GTK_ORIENTATION_VERTICAL,
        -1,
        &minimum_height,
        &natural_height,
        NULL,
        NULL);
    CHECK(minimum_width == (int)((metrics.cell_width + (unsigned int)scale - 1) /
                                 (unsigned int)scale));
    CHECK(minimum_height ==
          (int)((metrics.cell_height + (unsigned int)scale - 1) /
                (unsigned int)scale));
    CHECK(natural_width ==
          (int)((metrics.cell_width * terminal_config.columns +
                 (unsigned int)scale - 1) /
                (unsigned int)scale));
    CHECK(natural_height ==
          (int)((metrics.cell_height * terminal_config.rows +
                 (unsigned int)scale - 1) /
                (unsigned int)scale));

    width = natural_width;
    height = natural_height;
    CHECK(boxrig_gtk4_terminal_widget_set_window_state(
              widget,
              BOXRIG_UI_WINDOW_EMBEDDED) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    CHECK(frame_request_count == 0);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_resizable(window, false);
    gtk_window_set_default_size(window, width, height);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    g_signal_connect(
        widget,
        "map",
        G_CALLBACK(mark_widget_mapped),
        &mapped);
    gtk_window_present(window);
    while (!mapped) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(gtk_widget_get_mapped(GTK_WIDGET(widget)));
    /* Mapping and native focus input can independently request dispatch. */
    CHECK(frame_request_count >= 1);
    while (boxrig_gtk4_terminal_widget_get_layout(widget, &layout) !=
           BOXRIG_UI_OK) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_gtk4_terminal_widget_get_layout(widget, &layout) ==
          BOXRIG_UI_OK);
    if (layout.rows != terminal_config.rows ||
        layout.columns != terminal_config.columns) {
        CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
                  widget,
                  &metrics) == BOXRIG_FONT_CATALOG_OK);
        fprintf(
            stderr,
            "GTK layout mismatch: allocation=%dx%d scale=%d cell=%ux%u "
            "grid=%zux%zu expected=%zux%zu\n",
            gtk_widget_get_width(GTK_WIDGET(widget)),
            gtk_widget_get_height(GTK_WIDGET(widget)),
            gtk_widget_get_scale_factor(GTK_WIDGET(widget)),
            metrics.cell_width,
            metrics.cell_height,
            layout.columns,
            layout.rows,
            terminal_config.columns,
            terminal_config.rows);
    }
    CHECK(layout.rows == terminal_config.rows);
    CHECK(layout.columns == terminal_config.columns);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale > 0);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(metrics.output_scale == (float)scale);
    CHECK(feed_bytes(terminal, first_text, sizeof(first_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);
    CHECK(boxrig_gtk4_terminal_widget_has_presentation(widget));
    first_generation =
        boxrig_gtk4_terminal_widget_presentation_generation(widget);
    CHECK(first_generation != 0);
    CHECK(!boxrig_ui_view_peek_dirty(
        boxrig_gtk4_terminal_widget_view(widget),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));

    requests_before_unmap = frame_request_count;
    gtk_widget_set_visible(GTK_WIDGET(window), false);
    CHECK(!gtk_widget_get_mapped(GTK_WIDGET(widget)));
    CHECK(feed_bytes(terminal, remapped_text, sizeof(remapped_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(frame_request_count == requests_before_unmap);
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget))) {
        (void)g_main_context_iteration(NULL, true);
    }
    /* Restoring retained focus also queues native input. Both sources may
     * wake the host; remapping must request and present the retained dirty. */
    CHECK(frame_request_count > requests_before_unmap);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) >
          first_generation);
    first_generation =
        boxrig_gtk4_terminal_widget_presentation_generation(widget);

    CHECK(feed_bytes(terminal, second_text, sizeof(second_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_gtk4_terminal_widget_view(widget),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(snapshot_widget(GTK_WIDGET(window), GTK_WIDGET(widget)));
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) ==
          first_generation);
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_gtk4_terminal_widget_view(widget),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) >
          first_generation);
    CHECK(!boxrig_ui_view_peek_dirty(
        boxrig_gtk4_terminal_widget_view(widget),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));

    horizontal = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(widget));
    vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(widget));
    CHECK(horizontal != NULL && vertical != NULL);
    CHECK(gtk_adjustment_get_lower(horizontal) == 0.0);
    CHECK(gtk_adjustment_get_upper(horizontal) == 1.0);
    CHECK(gtk_adjustment_get_page_size(horizontal) == 1.0);
    CHECK(gtk_adjustment_get_page_size(vertical) ==
          (double)terminal_config.rows);
    font_specification = boxrig_font_specification_default();
    font_specification.size_dip = 15.0f;
    CHECK(boxrig_gtk4_terminal_widget_set_font_specification(
              widget,
              &font_specification) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_font_specification(
              widget,
              &reported_specification) == BOXRIG_UI_OK);
    CHECK(reported_specification.size_dip == 15.0f);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &changed_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed_metrics.requested_size_dip == 15.0f);
    CHECK(changed_metrics.output_scale == (float)scale);
    CHECK(changed_metrics.layout_revision != metrics.layout_revision);
    font_specification.size_dip = 0.0f;
    CHECK(boxrig_gtk4_terminal_widget_set_font_specification(
              widget,
              &font_specification) != BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &preserved_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(preserved_metrics.layout_revision == changed_metrics.layout_revision);
    CHECK(preserved_metrics.requested_size_dip == 15.0f);

    catalog_revision =
        boxrig_gtk4_terminal_widget_font_catalog_revision(widget);
    CHECK(catalog_revision != 0);
    CHECK(boxrig_gtk4_terminal_widget_register_font_file(
              widget,
              font_path,
              strlen(font_path),
              0,
              &file_resource) == BOXRIG_FONT_CATALOG_OK);
    CHECK(file_resource != BOXRIG_FONT_RESOURCE_NONE);
    CHECK(boxrig_gtk4_terminal_widget_font_catalog_revision(widget) ==
          catalog_revision + 1);
    font_data = read_font_file(font_path, &font_length);
    CHECK(font_data != NULL);
    CHECK(boxrig_gtk4_terminal_widget_register_font_memory(
              widget,
              font_data,
              font_length,
              0,
              &memory_resource) == BOXRIG_FONT_CATALOG_OK);
    free(font_data);
    font_data = NULL;
    CHECK(memory_resource != BOXRIG_FONT_RESOURCE_NONE);
    CHECK(memory_resource != file_resource);
    CHECK(boxrig_gtk4_terminal_widget_primary_font_resource(widget) ==
          BOXRIG_FONT_RESOURCE_NONE);
    memset(&resource_family, 0, sizeof(resource_family));
    resource_family.resource_id = file_resource;
    font_specification = boxrig_font_specification_default();
    font_specification.size_dip = 15.0f;
    font_specification.families = &resource_family;
    font_specification.family_count = 1;
    CHECK(boxrig_gtk4_terminal_widget_set_font_specification(
              widget,
              &font_specification) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_primary_font_resource(widget) ==
          file_resource);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &resource_metrics) == BOXRIG_FONT_CATALOG_OK);

    CHECK(boxrig_gtk4_terminal_widget_get_font_rendering_policy(
              widget,
              &rendering_policy) == BOXRIG_UI_OK);
    rendering_policy.dpi_x = 120.0f;
    rendering_policy.dpi_y = 120.0f;
    CHECK(boxrig_gtk4_terminal_widget_set_font_rendering_policy(
              widget,
              &rendering_policy) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_font_rendering_policy(
              widget,
              &reported_policy) == BOXRIG_UI_OK);
    CHECK(reported_policy.dpi_x == 120.0f);
    CHECK(reported_policy.dpi_y == 120.0f);
    CHECK(reported_policy.output_scale == (float)scale);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &policy_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(policy_metrics.layout_revision > resource_metrics.layout_revision);
    rendering_policy.output_scale = (float)scale + 1.0f;
    CHECK(boxrig_gtk4_terminal_widget_set_font_rendering_policy(
              widget,
              &rendering_policy) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &preserved_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(preserved_metrics.layout_revision == policy_metrics.layout_revision);

    CHECK(boxrig_gtk4_terminal_widget_set_line_height_adjustment(
              widget,
              2.0f) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_line_height_adjustment(
              widget,
              &line_height) == BOXRIG_UI_OK);
    CHECK(line_height == 2.0f);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &line_height_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(line_height_metrics.glyph_height == policy_metrics.glyph_height);
    CHECK(line_height_metrics.cell_height > policy_metrics.cell_height);
    CHECK(boxrig_gtk4_terminal_widget_set_text_direction(
              widget,
              BOXRIG_TEXT_DIRECTION_FORCE_RTL) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_text_direction(widget) ==
          BOXRIG_TEXT_DIRECTION_FORCE_RTL);

    zoom_policy = boxrig_font_visual_step_policy_default();
    zoom_policy.minimum_size_dip =
        line_height_metrics.requested_size_dip - 2.0f;
    zoom_policy.maximum_size_dip =
        line_height_metrics.requested_size_dip + 2.0f;
    CHECK(boxrig_gtk4_terminal_widget_visual_zoom(
              widget,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &zoom_changed) == BOXRIG_UI_OK);
    CHECK(zoom_changed);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &zoomed_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(zoomed_metrics.requested_size_dip >
          line_height_metrics.requested_size_dip);
    CHECK(boxrig_gtk4_terminal_widget_get_font_specification(
              widget,
              &reported_specification) == BOXRIG_UI_OK);
    CHECK(reported_specification.size_dip == 15.0f);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    layout_timeout = g_timeout_add_seconds(
        30, expire_layout_boundary_wait, &layout_expired);
    while (!layout_expired &&
           !boxrig_gtk4_terminal_widget_take_layout_action(
               widget, &layout_action)) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(!layout_expired);
    CHECK(g_source_remove(layout_timeout));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(boxrig_terminal_resize(
              terminal,
              layout_action.value.grid_resize.rows,
              layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(
              widget, layout_action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_visual_zoom_reset(
              widget,
              &zoom_changed) == BOXRIG_UI_OK);
    CHECK(zoom_changed);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &reset_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(reset_metrics.requested_size_dip ==
          line_height_metrics.requested_size_dip);
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.running);
    CHECK(diagnostic.visible);
    CHECK(diagnostic.layout_authority);
    CHECK(diagnostic.layout_configured);

    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    widget = NULL;
    widget_config.attachment = BOXRIG_UI_COMPONENT_LAYOUT_AUTHORITY;
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &replacement) == BOXRIG_UI_OK);
    g_object_ref_sink(replacement);
    g_object_unref(replacement);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_widget_zoom_commits_complete_resize_presentation(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkWindow *window;
    boxrig_font_effective_metrics metrics;
    boxrig_font_visual_step_policy zoom_policy;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_size_command command;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_size_outcome outcome;
    boxrig_ui_layout_result layout;
    boxrig_ui_fixed allocation_width;
    boxrig_ui_fixed allocation_height;
    graphene_rect_t bounds;
    uint64_t initial_generation;
    int initial_width;
    int initial_height;
    int target_width;
    int target_height;
    int scale;
    bool changed = false;
    bool applied = false;

    terminal_config.rows = 3;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale > 0);
    initial_width = (int)(
        (metrics.cell_width * terminal_config.columns +
         (unsigned int)scale - 1) /
        (unsigned int)scale);
    initial_height = (int)(
        (metrics.cell_height * terminal_config.rows +
         (unsigned int)scale - 1) /
        (unsigned int)scale);
    CHECK(boxrig_gtk4_terminal_widget_set_window_state(
              widget,
              BOXRIG_UI_WINDOW_NORMAL) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, initial_width, initial_height);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           gtk_widget_get_width(GTK_WIDGET(widget)) != initial_width ||
           gtk_widget_get_height(GTK_WIDGET(widget)) != initial_height) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    initial_generation =
        boxrig_gtk4_terminal_widget_presentation_generation(widget);
    CHECK(initial_generation != 0);

    zoom_policy = boxrig_font_visual_step_policy_default();
    zoom_policy.minimum_size_dip = metrics.requested_size_dip;
    zoom_policy.maximum_size_dip = metrics.requested_size_dip + 4.0f;
    CHECK(boxrig_gtk4_terminal_widget_visual_zoom(
              widget,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_gtk4_terminal_widget_get_size_command_state(
              widget,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(!take_content_size_action(widget, &command));
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.additional_dispatch);
    /* GTK may allocate the existing pixel extent again while the new font's
     * content-size request is ready but has not been dispatched. */
    gtk_widget_allocate(GTK_WIDGET(widget), initial_width, initial_height,
        -1, NULL);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    size_t preferred_rows, preferred_columns;
    CHECK(boxrig_gtk4_terminal_widget_get_preferred_grid(widget,
          &preferred_rows, &preferred_columns) == BOXRIG_UI_OK);
    CHECK(preferred_rows == terminal_config.rows &&
          preferred_columns == terminal_config.columns);
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) ==
          initial_generation);
    CHECK(take_content_size_action(widget, &command));
    CHECK(!take_content_size_action(widget, &command));
    CHECK(boxrig_gtk4_terminal_widget_get_size_command_state(
              widget,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED);
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) ==
          initial_generation);
    CHECK(command.width % BOXRIG_UI_FIXED_ONE == 0);
    CHECK(command.height % BOXRIG_UI_FIXED_ONE == 0);
    target_width = (int)(
        ((uint64_t)(command.width / BOXRIG_UI_FIXED_ONE) +
         (unsigned int)scale - 1) /
        (unsigned int)scale);
    target_height = (int)(
        ((uint64_t)(command.height / BOXRIG_UI_FIXED_ONE) +
         (unsigned int)scale - 1) /
        (unsigned int)scale);
    CHECK(target_width > initial_width && target_height > initial_height);

    gtk_window_set_default_size(window, target_width, target_height);
    while (gtk_widget_get_width(GTK_WIDGET(widget)) != target_width ||
           gtk_widget_get_height(GTK_WIDGET(widget)) != target_height) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) >
          initial_generation);
    CHECK(snapshot_widget_bounds(
        GTK_WIDGET(window),
        GTK_WIDGET(widget),
        &bounds));
    CHECK(bounds.origin.x == 0.0f && bounds.origin.y == 0.0f);
    CHECK(bounds.size.width == (float)target_width);
    CHECK(bounds.size.height == (float)target_height);
    allocation_width =
        (boxrig_ui_fixed)target_width * scale * BOXRIG_UI_FIXED_ONE;
    allocation_height =
        (boxrig_ui_fixed)target_height * scale * BOXRIG_UI_FIXED_ONE;
    outcome = allocation_width == command.width &&
                      allocation_height == command.height
                  ? BOXRIG_UI_SIZE_ACCEPTED
                  : BOXRIG_UI_SIZE_CONSTRAINED;
    CHECK(boxrig_gtk4_terminal_widget_complete_size_command(
              widget,
              command.revision,
              outcome,
              allocation_width,
              allocation_height,
              &applied,
              &layout) == BOXRIG_UI_OK);
    CHECK(applied);
    CHECK(boxrig_gtk4_terminal_widget_get_size_command_state(
              widget,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(layout.rows == terminal_config.rows);
    CHECK(layout.columns == terminal_config.columns);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    bool expired = false;
    guint guard = g_timeout_add_seconds(30, expire_layout_boundary_wait,
        &expired);
    size_t grid_completions = 0;
    for (size_t attempt = 0;
         attempt < 8 && !expired &&
             (frame_result.frame_pending || frame_result.additional_dispatch);
         attempt++) {
        boxrig_ui_layout_action action;
        if (boxrig_gtk4_terminal_widget_take_layout_action(widget, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(terminal,
                action.value.grid_resize.rows,
                action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(widget,
                action.revision, true) == BOXRIG_UI_OK);
            CHECK(++grid_completions <= 2);
        } else if (!frame_result.additional_dispatch) {
            (void)g_main_context_iteration(NULL, true);
        }
        CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
              BOXRIG_UI_OK);
    }
    CHECK(!expired);
    CHECK(g_source_remove(guard));
    CHECK(!frame_result.frame_pending && !frame_result.additional_dispatch);
    size_t final_rows, final_columns;
    boxrig_terminal_size(terminal, &final_rows, &final_columns);
    CHECK(final_rows == terminal_config.rows &&
          final_columns == terminal_config.columns);
    CHECK(boxrig_gtk4_terminal_widget_get_size_command_state(
              widget,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);

    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_constrained_zoom_uses_one_layout_action(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkWindow *window;
    boxrig_font_effective_metrics before;
    boxrig_font_effective_metrics after;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state size_state;
    uint64_t initial_generation;
    size_t frame_requests = 0;
    size_t rows;
    size_t columns;
    guint timeout;
    int width;
    int height;
    int scale;
    bool changed = false;
    bool expired = false;

    terminal_config.rows = 12;
    terminal_config.columns = 40;
    widget_config.frame_request_callback = count_frame_request;
    widget_config.frame_request_context = &frame_requests;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget, &before) == BOXRIG_FONT_CATALOG_OK);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale > 0);
    width = (int)((before.cell_width * terminal_config.columns +
                   (unsigned int)scale - 1) /
                  (unsigned int)scale);
    height = (int)((before.cell_height * terminal_config.rows +
                    (unsigned int)scale - 1) /
                   (unsigned int)scale);
    CHECK(boxrig_gtk4_terminal_widget_set_window_state(
              widget, BOXRIG_UI_WINDOW_NORMAL) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, width, height);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           gtk_widget_get_width(GTK_WIDGET(widget)) != width ||
           gtk_widget_get_height(GTK_WIDGET(widget)) != height) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    initial_generation =
        boxrig_gtk4_terminal_widget_presentation_generation(widget);
    CHECK(initial_generation != 0);

    CHECK(boxrig_gtk4_terminal_widget_set_window_state(
              widget, BOXRIG_UI_WINDOW_MAXIMIZED) == BOXRIG_UI_OK);
    gtk_widget_allocate(GTK_WIDGET(widget), width, height, -1, NULL);
    CHECK(boxrig_gtk4_terminal_widget_visual_zoom(
              widget,
              BOXRIG_FONT_ZOOM_DECREASE,
              boxrig_font_visual_step_policy_default(),
              &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget, &after) == BOXRIG_FONT_CATALOG_OK);
    CHECK(after.layout_revision > before.layout_revision);
    CHECK(boxrig_gtk4_terminal_widget_get_size_command_state(
              widget, &size_state) == BOXRIG_UI_OK);
    CHECK(size_state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(!boxrig_gtk4_terminal_widget_take_layout_action(widget, &action));
    CHECK(!boxrig_gtk4_terminal_widget_frame_ready(widget));
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) ==
          initial_generation);

    gtk_widget_set_visible(GTK_WIDGET(window), false);
    CHECK(!gtk_widget_get_mapped(GTK_WIDGET(widget)));
    CHECK(!boxrig_gtk4_terminal_widget_take_layout_action(widget, &action));
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget))) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(!boxrig_gtk4_terminal_widget_take_layout_action(widget, &action));
    timeout = g_timeout_add_seconds(30, expire_layout_boundary_wait, &expired);
    while (!expired &&
           !boxrig_gtk4_terminal_widget_take_layout_action(widget, &action)) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(!expired);
    CHECK(g_source_remove(timeout));
    CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(action.value.grid_resize.rows ==
          (size_t)(height * scale) / after.cell_height);
    CHECK(action.value.grid_resize.columns ==
          (size_t)(width * scale) / after.cell_width);
    CHECK(action.value.grid_resize.rows != terminal_config.rows ||
          action.value.grid_resize.columns != terminal_config.columns);
    CHECK(!boxrig_gtk4_terminal_widget_take_layout_action(widget, &action));
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
    CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(
              widget, action.revision, true) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resize(
              terminal,
              action.value.grid_resize.rows,
              action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(
              widget, action.revision, true) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame_ready(widget));
    CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(
              widget, action.revision, true) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_presentation_generation(widget) >
          initial_generation);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == action.value.grid_resize.rows);
    CHECK(columns == action.value.grid_resize.columns);

    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_widget_resize_preview_covers_expanded_allocation(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkWindow *window;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    uint32_t expanded_pixel = 0;
    int width;
    int height;

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_window_present(window);
    while (boxrig_gtk4_terminal_widget_get_layout(widget, &layout) !=
           BOXRIG_UI_OK) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);

    width = gtk_widget_get_width(GTK_WIDGET(widget));
    height = gtk_widget_get_height(GTK_WIDGET(widget));
    CHECK(width > 0 && height > 0);
    width += 23;
    height += 17;
    gtk_widget_allocate(GTK_WIDGET(widget), width, height, -1, NULL);
    CHECK(snapshot_widget_pixel(
        GTK_WIDGET(window),
        GTK_WIDGET(widget),
        width - 1,
        height - 1,
        &expanded_pixel));
    CHECK((expanded_pixel >> 24) == 255);

    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_sample_container_reserves_scrollbar_outside_grid(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkScrolledWindow *container;
    GtkWindow *window;
    GtkWidget *horizontal_scrollbar;
    GtkWidget *vertical_scrollbar;
    GtkAdjustment *vertical_adjustment;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_scrollbar_model scrollbar_model;
    boxrig_font_effective_metrics metrics;
    boxrig_font_effective_metrics wrapped_metrics;
    GtkPolicyType horizontal_policy;
    GtkPolicyType vertical_policy;
    int minimum_width;
    int natural_width;
    int minimum_height;
    int natural_height;
    int wrapped_minimum_width;
    int wrapped_natural_width;
    int wrapped_minimum_height;
    int wrapped_natural_height;
    int container_natural_width;
    int container_natural_height;
    size_t line;

    terminal_config.rows = 24;
    terminal_config.columns = 80;
    terminal_config.scrollback_capacity = 64;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    gtk_widget_measure(
        GTK_WIDGET(widget),
        GTK_ORIENTATION_HORIZONTAL,
        -1,
        &minimum_width,
        &natural_width,
        NULL,
        NULL);
    gtk_widget_measure(
        GTK_WIDGET(widget),
        GTK_ORIENTATION_VERTICAL,
        -1,
        &minimum_height,
        &natural_height,
        NULL,
        NULL);

    container = boxrig_gtk4_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    g_object_ref_sink(container);
    CHECK(GTK_IS_SCROLLED_WINDOW(container));
    CHECK(gtk_scrolled_window_get_child(container) == GTK_WIDGET(widget));
    gtk_scrolled_window_get_policy(
        container,
        &horizontal_policy,
        &vertical_policy);
    CHECK(horizontal_policy == GTK_POLICY_NEVER);
    CHECK(vertical_policy == GTK_POLICY_ALWAYS);
    CHECK(!gtk_scrolled_window_get_overlay_scrolling(container));
    CHECK(gtk_scrolled_window_get_propagate_natural_width(container));
    CHECK(gtk_scrolled_window_get_propagate_natural_height(container));
    CHECK(!gtk_scrolled_window_get_has_frame(container));
    CHECK(gtk_scrolled_window_get_placement(container) ==
          GTK_CORNER_TOP_LEFT);

    gtk_widget_measure(
        GTK_WIDGET(widget),
        GTK_ORIENTATION_HORIZONTAL,
        -1,
        &wrapped_minimum_width,
        &wrapped_natural_width,
        NULL,
        NULL);
    gtk_widget_measure(
        GTK_WIDGET(widget),
        GTK_ORIENTATION_VERTICAL,
        -1,
        &wrapped_minimum_height,
        &wrapped_natural_height,
        NULL,
        NULL);
    gtk_widget_measure(
        GTK_WIDGET(container),
        GTK_ORIENTATION_HORIZONTAL,
        -1,
        NULL,
        &container_natural_width,
        NULL,
        NULL);
    gtk_widget_measure(
        GTK_WIDGET(container),
        GTK_ORIENTATION_VERTICAL,
        -1,
        NULL,
        &container_natural_height,
        NULL,
        NULL);
    CHECK(wrapped_minimum_width == minimum_width);
    CHECK(wrapped_natural_width == natural_width);
    CHECK(wrapped_minimum_height == minimum_height);
    CHECK(wrapped_natural_height == natural_height);
    CHECK(container_natural_width > natural_width);
    CHECK(container_natural_height >= natural_height);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &wrapped_metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(wrapped_metrics.layout_revision == metrics.layout_revision);
    CHECK(wrapped_metrics.cell_width == metrics.cell_width);
    CHECK(wrapped_metrics.cell_height == metrics.cell_height);

    CHECK(boxrig_gtk4_terminal_widget_set_window_state(
              widget,
              BOXRIG_UI_WINDOW_NORMAL) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_resizable(window, false);
    gtk_window_set_default_size(
        window,
        container_natural_width,
        container_natural_height);
    gtk_window_set_child(window, GTK_WIDGET(container));
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           boxrig_gtk4_terminal_widget_get_layout(widget, &layout) !=
               BOXRIG_UI_OK) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(layout.rows == terminal_config.rows);
    CHECK(layout.columns == terminal_config.columns);
    horizontal_scrollbar = gtk_scrolled_window_get_hscrollbar(container);
    vertical_scrollbar = gtk_scrolled_window_get_vscrollbar(container);
    CHECK(horizontal_scrollbar != NULL && vertical_scrollbar != NULL);
    CHECK(GTK_IS_SCROLLBAR(horizontal_scrollbar));
    CHECK(GTK_IS_SCROLLBAR(vertical_scrollbar));
    CHECK(!gtk_widget_get_mapped(horizontal_scrollbar));
    CHECK(gtk_widget_get_mapped(vertical_scrollbar));
    CHECK(gtk_widget_get_width(GTK_WIDGET(container)) >=
          gtk_widget_get_width(GTK_WIDGET(widget)) +
              gtk_widget_get_width(vertical_scrollbar));
    vertical_adjustment = gtk_scrolled_window_get_vadjustment(container);
    CHECK(vertical_adjustment != NULL);
    CHECK(vertical_adjustment ==
          gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(widget)));
    CHECK(vertical_adjustment ==
          gtk_scrollbar_get_adjustment(GTK_SCROLLBAR(vertical_scrollbar)));
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);

    for (line = 0; line < 32; line++) {
        char text[16];
        int length = snprintf(text, sizeof(text), "%02zu\r\n", line);

        CHECK(length > 0 && (size_t)length < sizeof(text));
        CHECK(feed_bytes(terminal, (const uint8_t *)text, (size_t)length));
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_model(
              widget,
              &scrollbar_model) == BOXRIG_UI_OK);
    CHECK(scrollbar_model.enabled);
    CHECK(scrollbar_model.history_rows > 0);
    CHECK(scrollbar_model.position == scrollbar_model.history_rows);
    CHECK(gtk_adjustment_get_upper(vertical_adjustment) >
          gtk_adjustment_get_page_size(vertical_adjustment));
    gtk_adjustment_set_value(vertical_adjustment, 0.0);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_model(
              widget,
              &scrollbar_model) == BOXRIG_UI_OK);
    CHECK(scrollbar_model.position == 0);
    CHECK(scrollbar_model.viewport_offset == scrollbar_model.history_rows);

    gtk_window_set_child(window, NULL);
    gtk_scrolled_window_set_child(container, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(container);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_widget_input_fifo_and_backpressure(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkWindow *window;
    GtkEventControllerKey *key_controller;
    GtkGestureClick *click_gesture;
    GtkEventControllerMotion *motion_controller;
    GtkIMContext *im_context;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_hit_test_result event_hit;
    boxrig_ui_hit_test_result current_hit;
    boxrig_text_cluster_input clusters[8];
    boxrig_text_layout *rtl_layout = NULL;
    key_route_observer observer;
    mouse_route_observer mouse_observer;
    const uint8_t *output = NULL;
    double native_x;
    double native_y;
    unsigned int scale;
    size_t index;

    memset(&observer, 0, sizeof(observer));
    memset(&mouse_observer, 0, sizeof(mouse_observer));
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    terminal_config.output_capacity = 3;
    widget_config.paste_chunk_size = 3;
    widget_config.maximum_native_input_events = 4;
    widget_config.maximum_frame_items = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 160, 80);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           boxrig_gtk4_terminal_widget_get_layout(widget, &layout) ==
               BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale != 0);
    native_x = ((double)metrics.cell_width + 0.25) / (double)scale;
    native_y = 0.25 / (double)scale;
    CHECK(boxrig_gtk4_terminal_widget_hit_test(
              widget,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE + 16,
              16,
              &event_hit) == BOXRIG_UI_OK);
    CHECK(event_hit.inside_grid && event_hit.logical_column == 1);

    click_gesture = GTK_GESTURE_CLICK(find_widget_controller(
        GTK_WIDGET(widget),
        GTK_TYPE_GESTURE_CLICK));
    motion_controller = GTK_EVENT_CONTROLLER_MOTION(find_widget_controller(
        GTK_WIDGET(widget),
        GTK_TYPE_EVENT_CONTROLLER_MOTION));
    CHECK(click_gesture != NULL && motion_controller != NULL);
    CHECK(boxrig_gtk4_terminal_widget_set_mouse_router(
              widget,
              capture_left_drag,
              &mouse_observer) == BOXRIG_UI_OK);
    CHECK(feed_bytes(
        terminal,
        (const uint8_t *)"\x1b[?1002h\x1b[?1006h",
        sizeof("\x1b[?1002h\x1b[?1006h") - 1));
    current_event_state_overridden = true;
    overridden_current_event_state = GDK_CONTROL_MASK;
    g_signal_emit_by_name(click_gesture, "pressed", 1, native_x, native_y);
    overridden_current_event_state = GDK_SHIFT_MASK;
    g_signal_emit_by_name(motion_controller, "motion", native_x, native_y);
    overridden_current_event_state = 0;
    g_signal_emit_by_name(click_gesture, "released", 1, native_x, native_y);
    current_event_state_overridden = false;
    for (index = 0; index < 8; index++) {
        memset(&clusters[index], 0, sizeof(clusters[index]));
        clusters[index].logical_column = index;
        clusters[index].column_count = 1;
        clusters[index].text_offset = index;
        clusters[index].text_length = 1;
        clusters[index].bidi_level = 1;
        clusters[index].style_key = 1;
        clusters[index].font_key = 1;
        clusters[index].script_key = 1;
        clusters[index].language_key = 1;
    }
    CHECK(boxrig_text_layout_create(
              clusters,
              8,
              8,
              BOXRIG_TEXT_DIRECTION_RTL,
              &rtl_layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(
              boxrig_gtk4_terminal_widget_view(widget),
              0,
              rtl_layout,
              512,
              999) == BOXRIG_UI_OK);
    rtl_layout = NULL;
    CHECK(boxrig_gtk4_terminal_widget_hit_test(
              widget,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE + 16,
              16,
              &current_hit) == BOXRIG_UI_OK);
    CHECK(current_hit.logical_column == 6);
    CHECK(current_hit.layout_generation != event_hit.layout_generation);
    CHECK(boxrig_gtk4_terminal_widget_set_mouse_router(
              widget,
              NULL,
              NULL) == BOXRIG_UI_BUSY);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    CHECK(mouse_observer.count == 3);
    CHECK(mouse_observer.events[0].action == BOXRIG_MOUSE_PRESS);
    CHECK(mouse_observer.events[1].action == BOXRIG_MOUSE_MOTION);
    CHECK(mouse_observer.events[2].action == BOXRIG_MOUSE_RELEASE);
    CHECK(mouse_observer.events[0].modifiers == BOXRIG_MODIFIER_CONTROL);
    CHECK(mouse_observer.events[1].modifiers == BOXRIG_MODIFIER_SHIFT);
    CHECK(mouse_observer.events[2].modifiers == 0u);
    CHECK(mouse_observer.events[0].x == (int64_t)metrics.cell_width);
    CHECK(mouse_observer.hits[0].x ==
          (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE + 16);
    CHECK(mouse_observer.hits[0].y == 16);
    CHECK(mouse_observer.hits[0].logical_column == 1);
    CHECK(mouse_observer.hits[0].layout_generation ==
          event_hit.layout_generation);
    CHECK(mouse_observer.hits[0].layout_generation !=
          current_hit.layout_generation);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    mouse_observer.count = 0;
    g_signal_emit_by_name(click_gesture, "pressed", 1, -0.001 / scale, -0.5 / scale);
    g_signal_emit_by_name(click_gesture, "released", 1, -0.001 / scale, -0.5 / scale);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(mouse_observer.count == 2);
    CHECK(mouse_observer.events[0].x == -1 && mouse_observer.events[0].y == -1);
    CHECK(!mouse_observer.hits[0].inside_grid && mouse_observer.hits[0].x < 0);
    CHECK(boxrig_gtk4_terminal_widget_set_mouse_router(widget, NULL, NULL) ==
          BOXRIG_UI_OK);

    key_controller = GTK_EVENT_CONTROLLER_KEY(find_widget_controller(
        GTK_WIDGET(widget),
        GTK_TYPE_EVENT_CONTROLLER_KEY));
    CHECK(key_controller != NULL);
    im_context = latest_im_context;
    CHECK(GTK_IS_IM_MULTICONTEXT(im_context));
    CHECK(boxrig_gtk4_terminal_widget_composition(widget) != NULL);
    {
        static const uint8_t pending[] = "\033[?2026hX";
        static const uint8_t finish[] = "\033[?2026l";
        size_t requests = 0;

        /* Ctrl-C must still wake input processing while drawing is held. */
        CHECK(feed_bytes(terminal, pending, sizeof(pending) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
        CHECK(boxrig_gtk4_terminal_widget_set_frame_request_callback(
                  widget, count_frame_request, &requests) == BOXRIG_UI_OK);
        CHECK(emit_key_press(key_controller, GDK_KEY_c, 54, GDK_CONTROL_MASK));
        CHECK(requests != 0);
        emit_key_release(key_controller, GDK_KEY_c, 54, GDK_CONTROL_MASK);
        CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
        CHECK(output[0] == 3);
        CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
        CHECK(feed_bytes(terminal, finish, sizeof(finish) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(!boxrig_terminal_synchronized_output(terminal));
        CHECK(boxrig_gtk4_terminal_widget_set_frame_request_callback(
                  widget, NULL, NULL) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_gtk4_terminal_widget_set_key_router(
              widget,
              capture_key_series,
              &observer) == BOXRIG_UI_OK);

    CHECK(emit_key_press(key_controller, GDK_KEY_a, 38, 0));
    CHECK(emit_key_press(key_controller, GDK_KEY_a, 38, 0));
    emit_key_release(key_controller, GDK_KEY_a, 38, 0);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.native_input_queue_depth == 3);
    CHECK(diagnostic.native_input_queue_high_water == 3);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(observer.count == 3);
    CHECK(observer.actions[0] == BOXRIG_KEY_PRESS);
    CHECK(observer.actions[1] == BOXRIG_KEY_REPEAT);
    CHECK(observer.actions[2] == BOXRIG_KEY_RELEASE);
    CHECK(observer.events[0].codepoint == 'a');
    CHECK(observer.events[0].shifted_codepoint == 'A');
    CHECK(observer.events[0].base_layout_codepoint == 'a');
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(emit_key_press(
        key_controller,
        GDK_KEY_question,
        61,
        GDK_SHIFT_MASK));
    emit_key_release(
        key_controller,
        GDK_KEY_question,
        61,
        GDK_SHIFT_MASK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(observer.count == 5);
    CHECK(observer.events[3].codepoint == '/');
    CHECK(observer.events[3].shifted_codepoint == '?');
    CHECK(observer.events[3].base_layout_codepoint == '/');
    CHECK(observer.events[3].modifiers == BOXRIG_MODIFIER_SHIFT);
    CHECK(observer.events[4].codepoint == '/');
    CHECK(observer.events[4].shifted_codepoint == '?');
    CHECK(observer.events[4].base_layout_codepoint == '/');
    CHECK(boxrig_gtk4_terminal_widget_set_key_router(widget, NULL, NULL) ==
          BOXRIG_UI_OK);

    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(emit_key_press(key_controller, GDK_KEY_Up, 111, 0));
    CHECK(emit_key_press(key_controller, GDK_KEY_b, 56, 0));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(frame_result.additional_dispatch);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "\x1b[A", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "b", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    emit_key_release(key_controller, GDK_KEY_Up, 111, 0);
    emit_key_release(key_controller, GDK_KEY_b, 56, 0);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);

    CHECK(emit_key_press(
        key_controller,
        GDK_KEY_c,
        54,
        GDK_CONTROL_MASK));
    emit_key_release(key_controller, GDK_KEY_c, 54, GDK_CONTROL_MASK);
    CHECK(emit_key_press(
        key_controller,
        GDK_KEY_z,
        52,
        GDK_CONTROL_MASK));
    emit_key_release(key_controller, GDK_KEY_z, 52, GDK_CONTROL_MASK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 2);
    CHECK(memcmp(output, "\x03\x1a", 2) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);

    g_signal_emit_by_name(im_context, "commit", "\xe6\x97\xa5");
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "\xe6\x97\xa5", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);

    CHECK(boxrig_gtk4_terminal_widget_queue_paste(
              widget,
              (const uint8_t *)"one\r\ntwo",
              8) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "one", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "\ntw", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "o", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    CHECK(emit_key_press(key_controller, GDK_KEY_1, 10, 0));
    CHECK(emit_key_press(key_controller, GDK_KEY_2, 11, 0));
    CHECK(emit_key_press(key_controller, GDK_KEY_3, 12, 0));
    CHECK(emit_key_press(key_controller, GDK_KEY_4, 13, 0));
    CHECK(emit_key_press(key_controller, GDK_KEY_5, 14, 0));
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.failed);
    CHECK(diagnostic.failure_reason ==
          BOXRIG_UI_COMPONENT_FAILURE_NATIVE_INPUT_OVERFLOW);
    CHECK(!diagnostic.running);
    CHECK(diagnostic.native_input_queue_depth == 4);
    CHECK(diagnostic.native_input_queue_high_water == 4);
    CHECK(diagnostic.native_input_queue_capacity == 4);

    g_object_unref(key_controller);
    g_object_unref(click_gesture);
    g_object_unref(motion_controller);
    CHECK(boxrig_gtk4_terminal_widget_quiesce(widget) == BOXRIG_UI_OK);
    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_widget_input_lifecycle_releases_context(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkWindow *window;
    GtkEventControllerKey *key_controller;
    GtkIMContext *im_context;
    boxrig_composition *composition;
    boxrig_composition_view composition_before;
    boxrig_composition_view composition_after;
    boxrig_ui_layout_result layout;
    const uint8_t *output = NULL;
    const char *expected_context_id;
    const char *context_id;
    GWeakRef widget_reference;
    GWeakRef context_reference;

    terminal_config.rows = 2;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 160, 80);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           boxrig_gtk4_terminal_widget_get_layout(widget, &layout) ==
               BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }

    key_controller = GTK_EVENT_CONTROLLER_KEY(find_widget_controller(
        GTK_WIDGET(widget),
        GTK_TYPE_EVENT_CONTROLLER_KEY));
    CHECK(key_controller != NULL);
    im_context = latest_im_context;
    CHECK(GTK_IS_IM_MULTICONTEXT(im_context));
    composition = boxrig_gtk4_terminal_widget_composition(widget);
    CHECK(composition != NULL);
    g_weak_ref_init(&widget_reference, G_OBJECT(widget));
    g_weak_ref_init(&context_reference, G_OBJECT(im_context));

    gtk_window_set_focus(window, GTK_WIDGET(widget));
    while (g_main_context_iteration(NULL, false)) {
    }
    CHECK(gtk_window_get_focus(window) == GTK_WIDGET(widget));
    CHECK(boxrig_ui_view_focused(boxrig_gtk4_terminal_widget_view(widget)));
    expected_context_id = getenv("BOXRIG_TEST_GTK4_IM_MODULE");
    context_id = gtk_im_multicontext_get_context_id(
        GTK_IM_MULTICONTEXT(im_context));
    CHECK(expected_context_id != NULL && expected_context_id[0] != '\0');
    CHECK(context_id != NULL && strcmp(context_id, expected_context_id) == 0);

    gtk_window_set_focus(window, NULL);
    while (g_main_context_iteration(NULL, false)) {
    }
    CHECK(gtk_window_get_focus(window) != GTK_WIDGET(widget));
    CHECK(!boxrig_ui_view_focused(boxrig_gtk4_terminal_widget_view(widget)));
    gtk_window_set_child(window, NULL);
    while (g_main_context_iteration(NULL, false)) {
    }
    CHECK(gtk_widget_get_root(GTK_WIDGET(widget)) == NULL);

    gtk_window_set_child(window, GTK_WIDGET(widget));
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget))) {
        (void)g_main_context_iteration(NULL, true);
    }
    gtk_window_set_focus(window, GTK_WIDGET(widget));
    while (g_main_context_iteration(NULL, false)) {
    }
    CHECK(gtk_window_get_focus(window) == GTK_WIDGET(widget));
    gtk_window_set_focus(window, NULL);
    while (g_main_context_iteration(NULL, false)) {
    }

    CHECK(boxrig_ui_view_set_focused(
              boxrig_gtk4_terminal_widget_view(widget), true) == BOXRIG_UI_OK);
    /* Focus and root changes may replace the input context. Exercise late
     * signals and final ownership against the currently attached context. */
    im_context = latest_im_context;
    CHECK(GTK_IS_IM_MULTICONTEXT(im_context));
    g_weak_ref_clear(&context_reference);
    g_weak_ref_init(&context_reference, G_OBJECT(im_context));
    CHECK(boxrig_gtk4_terminal_widget_quiesce(widget) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_focused(boxrig_gtk4_terminal_widget_view(widget)));
    CHECK(boxrig_gtk4_terminal_widget_quiesce(widget) == BOXRIG_UI_OK);
    CHECK(boxrig_composition_get_view(composition, &composition_before) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(composition_before.terminated);
    g_signal_emit_by_name(im_context, "commit", "ignored");
    g_signal_emit_by_name(im_context, "preedit-changed");
    g_signal_emit_by_name(im_context, "preedit-end");
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_composition_get_view(composition, &composition_after) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(composition_after.last_event == composition_before.last_event);
    CHECK(composition_after.active == composition_before.active);
    CHECK(composition_after.commit_pending ==
          composition_before.commit_pending);
    CHECK(composition_after.terminated == composition_before.terminated);
    CHECK(composition_after.text_length == composition_before.text_length);

    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(key_controller);
    g_object_unref(widget);
    CHECK(weak_reference_is_empty(&widget_reference));
    CHECK(weak_reference_is_empty(&context_reference));
    g_weak_ref_clear(&widget_reference);
    g_weak_ref_clear(&context_reference);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_widget_clipboard_actions(void)
{
    static const uint8_t copied_text[] = "copy";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk4_terminal_widget_config widget_config =
        boxrig_gtk4_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    GtkWindow *window;
    GdkDisplay *display;
    GdkClipboard *primary;
    GdkClipboard *clipboard;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_layout_result layout;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_ui_view_action action;
    clipboard_read_result read_result;
    const uint8_t *output = NULL;
    size_t output_length;

    terminal_config.rows = 2;
    terminal_config.columns = 8;
    widget_config.paste_capacity = 4;
    widget_config.paste_chunk_size = 2;
    widget_config.maximum_clipboard_transfers = 2;
    widget_config.maximum_clipboard_bytes = 10;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new());
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 160, 80);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           boxrig_gtk4_terminal_widget_get_layout(widget, &layout) ==
               BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(feed_bytes(terminal, copied_text, sizeof(copied_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_gtk4_terminal_widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_hit_test(
              widget,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 2,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.text_position_valid);
    CHECK(boxrig_ui_view_begin_selection(
              boxrig_gtk4_terminal_widget_view(widget),
              BOXRIG_UI_SELECTION_LINE,
              &hit) == BOXRIG_UI_OK);

    display = gtk_widget_get_display(GTK_WIDGET(widget));
    CHECK(display != NULL);
    primary = gdk_display_get_primary_clipboard(display);
    clipboard = gdk_display_get_clipboard(display);
    CHECK(primary != NULL && clipboard != NULL && primary != clipboard);
    capabilities =
        boxrig_gtk4_terminal_widget_view_action_capabilities(widget);
    CHECK(capabilities.copy_targets ==
          (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
           BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    capabilities = boxrig_gtk4_terminal_widget_view_action_capabilities(widget);
    CHECK(capabilities.paste_targets == 0 && !capabilities.reconversion);

    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == 10);

    memset(&read_result, 0, sizeof(read_result));
    gdk_clipboard_read_text_async(
        primary,
        NULL,
        clipboard_text_read,
        &read_result);
    while (!read_result.completed) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(read_result.error == NULL && read_result.text != NULL);
    CHECK(strcmp(read_result.text, "copy\n") == 0);
    g_free(read_result.text);
    memset(&read_result, 0, sizeof(read_result));
    gdk_clipboard_read_text_async(
        clipboard,
        NULL,
        clipboard_text_read,
        &read_result);
    while (!read_result.completed) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(read_result.error == NULL && read_result.text != NULL);
    CHECK(strcmp(read_result.text, "copy\n") == 0);
    g_free(read_result.text);

    CHECK(boxrig_gtk4_terminal_widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
    gdk_clipboard_set_text(primary, "P\r\n");
    gdk_clipboard_set_text(clipboard, "P\r\n");
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == 0);
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_BUSY);
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.active_clipboard_transfers == 2);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(
                  widget,
                  &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(diagnostic.native_input_queue_depth == 2);
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof("P\nP\n") - 1);
    CHECK(memcmp(output, "P\nP\n", output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    /* Complete an old asynchronous read only after a stop/resume boundary. */
    gdk_clipboard_set_text(clipboard, "late");
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.active_clipboard_transfers == 1);
    CHECK(boxrig_gtk4_terminal_widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_gtk4_terminal_widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    gdk_clipboard_set_text(clipboard, "drop");
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    gtk_widget_set_visible(GTK_WIDGET(window), false);
    CHECK(!gtk_widget_get_mapped(GTK_WIDGET(widget)));
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(
                  widget,
                  &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_UNAVAILABLE);
    gtk_window_present(window);
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget))) {
        (void)g_main_context_iteration(NULL, true);
    }

    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    gdk_clipboard_set_text(primary, "12345");
    CHECK(boxrig_gtk4_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(
                  widget,
                  &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(diagnostic.failed);
    CHECK(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_CLIPBOARD);

    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}


static bool settle_scrollbar_visibility(BoxrigGtk4TerminalWidget *widget, boxrig_terminal *terminal,
    unsigned int *resizes_out)
{
    *resizes_out = 0;
    boxrig_ui_layout_result initial;
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
        boxrig_gtk4_terminal_widget_get_layout(widget, &initial) != BOXRIG_UI_OK)
        (void)g_main_context_iteration(NULL, true);
    for (;;) {
        while (g_main_context_pending(NULL)) (void)g_main_context_iteration(NULL, false);
        boxrig_ui_component_frame_result frame;
        boxrig_ui_status status = boxrig_gtk4_terminal_widget_frame(widget, &frame);
        if (status != BOXRIG_UI_OK) {
            boxrig_gtk_render_state rendering;
            CHECK(boxrig_gtk4_terminal_widget_get_render_state(widget, &rendering) == BOXRIG_UI_OK);
            fprintf(stderr, "GTK 4 frame failed: status=%d reason=%d gl=%u message=%s\n",
                status, rendering.reason, rendering.gl_code, rendering.message);
        }
        CHECK(status == BOXRIG_UI_OK);
        boxrig_ui_layout_action action;
        if (boxrig_gtk4_terminal_widget_take_layout_action(widget, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
                action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(widget, action.revision, true) == BOXRIG_UI_OK);
            CHECK(++*resizes_out < 4);
            continue;
        }
        boxrig_ui_scrollbar_visibility_state state;
        CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
        if (gtk_widget_get_mapped(GTK_WIDGET(widget)) && !state.pending &&
            !frame.frame_pending) break;
        (void)g_main_context_iteration(NULL, true);
    }
    return true;
}

static gboolean scrollbar_scale_expired(gpointer data)
{
    *(bool *)data = true;
    return G_SOURCE_REMOVE;
}

static bool test_scrollbar_visibility_during_scale_change(void)
{
    const char *backend = getenv("GDK_BACKEND");
    if (backend == NULL || strcmp(backend, "x11") != 0 || getenv("GDK_SCALE") != NULL) return true;
    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    char selection_name[40];
    CHECK(snprintf(selection_name, sizeof(selection_name), "_XSETTINGS_S%d", DefaultScreen(display)) > 0);
    Atom selection = XInternAtom(display, selection_name, False);
    Atom property = XInternAtom(display, "_XSETTINGS_SETTINGS", False);
    CHECK(XGetSelectionOwner(display, selection) == None);
    Window owner = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display, owner, PropertyChangeMask);
    const char setting_name[] = "Gdk/WindowScalingFactor";
    uint16_t name_length = sizeof(setting_name) - 1;
    uint32_t one = 1;
    unsigned char settings[48] = {0};
    settings[0] = *(const unsigned char *)&one == 1 ? LSBFirst : MSBFirst;
    memcpy(settings + 8, &one, sizeof(one));
    memcpy(settings + 14, &name_length, sizeof(name_length));
    memcpy(settings + 16, setting_name, name_length);

    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    boxrig_gtk4_terminal_widget_config wc = boxrig_gtk4_terminal_widget_config_default();
    if (getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL) wc.render_policy = BOXRIG_GTK_RENDER_OPENGL_REQUIRED;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkScrolledWindow *container = boxrig_gtk4_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 420, 200);
    gtk_window_set_child(window, GTK_WIDGET(container));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_window_present(window);
    unsigned int resizes;
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    for (unsigned int index = 0; index < 10; index++) {
        boxrig_font_effective_metrics before, after;
        CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(widget, &before) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_scrollbar_visibility policy = index % 2 == 0 ? BOXRIG_UI_SCROLLBAR_NEVER : BOXRIG_UI_SCROLLBAR_ALWAYS;
        CHECK(boxrig_gtk4_terminal_widget_set_scrollbar_visibility(widget, policy) == BOXRIG_UI_OK);
        boxrig_ui_scrollbar_visibility_state bar;
        CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &bar) == BOXRIG_UI_OK);
        CHECK(bar.pending);
        boxrig_font_specification font = boxrig_font_specification_default();
        CHECK(boxrig_gtk4_terminal_widget_set_font_specification(widget, &font) == BOXRIG_UI_BUSY);
        bool changed = true;
        CHECK(boxrig_gtk4_terminal_widget_visual_zoom(widget, BOXRIG_FONT_ZOOM_INCREASE,
            boxrig_font_visual_step_policy_default(), &changed) == BOXRIG_UI_BUSY);
        CHECK(!changed);
        CHECK(boxrig_gtk4_terminal_widget_visual_zoom_reset(widget, &changed) == BOXRIG_UI_BUSY);
        uint32_t serial = index + 1;
        int32_t scale = index % 2 == 0 ? 2 : 1;
        memcpy(settings + 4, &serial, sizeof(serial));
        memcpy(settings + 40, &serial, sizeof(serial));
        memcpy(settings + 44, &scale, sizeof(scale));
        XChangeProperty(display, owner, property, property, 8, PropModeReplace, settings, sizeof(settings));
        if (index == 0) {
            XEvent timestamp;
            XEvent announcement = {0};
            XWindowEvent(display, owner, PropertyChangeMask, &timestamp);
            CHECK(timestamp.type == PropertyNotify);
            XSetSelectionOwner(display, selection, owner, timestamp.xproperty.time);
            CHECK(XGetSelectionOwner(display, selection) == owner);
            announcement.xclient.type = ClientMessage;
            announcement.xclient.window = DefaultRootWindow(display);
            announcement.xclient.message_type = XInternAtom(display, "MANAGER", False);
            announcement.xclient.format = 32;
            announcement.xclient.data.l[0] = (long)timestamp.xproperty.time;
            announcement.xclient.data.l[1] = (long)selection;
            announcement.xclient.data.l[2] = (long)owner;
            CHECK(XSendEvent(display, DefaultRootWindow(display), False, StructureNotifyMask, &announcement) != 0);
        }
        XFlush(display);
        bool expired = false;
        guint guard = g_timeout_add_seconds(30, scrollbar_scale_expired, &expired);
        while (!expired && gtk_widget_get_scale_factor(GTK_WIDGET(widget)) != scale)
            (void)g_main_context_iteration(NULL, true);
        if (!expired) g_source_remove(guard);
        CHECK(!expired);
        CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(widget, &after) == BOXRIG_FONT_CATALOG_OK);
        CHECK(after.layout_revision == before.layout_revision);
        CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
        CHECK(resizes > 0 && resizes <= 2);
        CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &bar) == BOXRIG_UI_OK);
        CHECK(!bar.pending && bar.visible == (policy == BOXRIG_UI_SCROLLBAR_ALWAYS));
        CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(widget, &after) == BOXRIG_FONT_CATALOG_OK);
        CHECK(after.layout_revision != before.layout_revision);
        size_t rows, columns;
        boxrig_terminal_size(terminal, &rows, &columns);
        CHECK(columns == (size_t)gtk_widget_get_width(GTK_WIDGET(widget)) * scale / after.cell_width);
        CHECK(rows == (size_t)gtk_widget_get_height(GTK_WIDGET(widget)) * scale / after.cell_height);
        CHECK(bar.reserved_width == (unsigned int)(gtk_widget_get_width(GTK_WIDGET(container)) -
            gtk_widget_get_width(GTK_WIDGET(widget))));
    }
    if (getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL) {
        boxrig_gtk_render_state state;
        CHECK(boxrig_gtk4_terminal_widget_get_render_state(widget, &state) == BOXRIG_UI_OK);
        CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    }
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    while (g_main_context_pending(NULL)) (void)g_main_context_iteration(NULL, false);
    puts("PASS GTK 4 scrollbar transaction survives native scale 1 -> 2 -> 1");
    return true;
}

static bool test_standard_scrollbar_visibility(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 8; config.columns = 30;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    boxrig_gtk4_terminal_widget_config wc = boxrig_gtk4_terminal_widget_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkScrolledWindow *container = boxrig_gtk4_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    g_object_ref_sink(container);
    CHECK(boxrig_gtk4_terminal_widget_create_scroll_container(widget) == NULL);
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 420, 200);
    gtk_window_set_child(window, GTK_WIDGET(container));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_window_present(window);
    unsigned int resizes;
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    boxrig_ui_scrollbar_visibility_state state;
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.visible && state.requested == BOXRIG_UI_SCROLLBAR_ALWAYS && state.reserved_width > 0);
    const int original_width = gtk_widget_get_width(GTK_WIDGET(widget));
    const int outer_width = gtk_widget_get_width(GTK_WIDGET(container));
    CHECK((unsigned int)(outer_width - original_width) == state.reserved_width);
    CHECK(boxrig_gtk4_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.pending && state.visible);
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 1);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
    CHECK(gtk_widget_get_width(GTK_WIDGET(widget)) == outer_width);
    size_t rows, columns;
    boxrig_terminal_size(terminal, &rows, &columns);
    boxrig_ui_layout_result layout;
    CHECK(boxrig_gtk4_terminal_widget_get_layout(widget, &layout) == BOXRIG_UI_OK);
    CHECK(rows == layout.rows && columns == layout.columns);
    CHECK(boxrig_gtk4_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_AUTO) == BOXRIG_UI_OK);
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 0);
    for (size_t i = 0; i < 30; i++) CHECK(feed_bytes(terminal, (const uint8_t *)"row\r\n", 5));
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 1);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.visible && state.reserved_width > 0 && !state.pending);
    CHECK(gtk_widget_get_width(GTK_WIDGET(widget)) == original_width);
    CHECK(boxrig_gtk4_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_OK);
    CHECK(feed_bytes(terminal, (const uint8_t *)"\033[?1049h", 8));
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 0);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(container);
    CHECK(gtk_adjustment_get_upper(adjustment) == gtk_adjustment_get_page_size(adjustment));
    CHECK(gtk_adjustment_get_value(adjustment) == 0);
    CHECK(feed_bytes(terminal, (const uint8_t *)"\033[?1049l", 8));
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(boxrig_gtk4_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_AUTO) == BOXRIG_UI_OK);
    const char *sequences[] = {"\033[?1049h", "\033[?1049l", "\033[3J"};
    for (size_t i = 0; i < 3; i++) {
        CHECK(feed_bytes(terminal, (const uint8_t *)sequences[i], strlen(sequences[i])));
        CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
        CHECK(resizes == 1);
        CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
        CHECK(state.visible == (i == 1) && !state.pending);
    }
    gtk_scrolled_window_set_child(container, NULL);
    gtk_window_set_child(window, NULL);
    g_object_unref(container);
    /* A caller-owned container remains entirely under the caller's policy. */
    GtkScrolledWindow *external = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    g_object_ref_sink(external);
    gtk_scrolled_window_set_policy(external, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_child(external, GTK_WIDGET(widget));
    CHECK(boxrig_gtk4_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
    GtkPolicyType horizontal, vertical;
    gtk_scrolled_window_get_policy(external, &horizontal, &vertical);
    CHECK(vertical == GTK_POLICY_ALWAYS);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
    gtk_scrolled_window_set_child(external, NULL);
    g_object_unref(external);
    container = boxrig_gtk4_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    g_object_ref_sink(container);
    CHECK(boxrig_gtk4_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.requested == BOXRIG_UI_SCROLLBAR_NEVER && !state.visible && !state.pending);
    gtk_scrolled_window_set_child(container, NULL);
    g_object_unref(container);
    gtk_window_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    printf("PASS GTK 4 standard scrollbar visibility, native width, single resize and external ownership\n");
    return true;
}

/* Protocol decorations must not invalidate otherwise usable preedit text. */
static bool test_preedit_clause_attributes(void)
{
    static const char text[] = "A記者の🙂汽車がZ";
    boxrig_gtk4_preedit_translation translation;
    PangoAttrList *attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_foreground_new(65535, 65535, 65535), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 14, 23);
    /* Reproduce malformed old GTK Wayland endpoints and a split UTF-8 scalar. */
    add_preedit_attribute(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD), 3, 1);
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 2, 3);
    CHECK(boxrig_gtk4_input_translate_preedit(text, attributes, 5, &translation));
    CHECK(translation.caret_offset == 14);
    CHECK(translation.selection_offset == 14 && translation.selection_length == 9);
    for (size_t index = 0; index < translation.segment_count; index++) {
        const boxrig_composition_segment *segment = &translation.segments[index];
        bool selected = segment->text_offset >= 14 && segment->text_offset < 23;
        CHECK((segment->feedback & BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT) == 0);
        CHECK((segment->feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0);
        CHECK(((segment->feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) == selected);
        CHECK(((segment->feedback & BOXRIG_COMPOSITION_FEEDBACK_REVERSE) != 0) == selected);
    }
    CHECK(translation.segment_count != 0);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 1, 7);
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 14, 23);
    CHECK(boxrig_gtk4_input_translate_preedit(text, attributes, 1, &translation));
    CHECK(translation.selection_length == 0);
    CHECK(translation.segment_count == 2);
    CHECK(translation.segments[0].text_offset == 1 && translation.segments[0].text_length == 6);
    CHECK(translation.segments[1].text_offset == 14 && translation.segments[1].text_length == 9);
    for (size_t index = 0; index < translation.segment_count; index++)
        CHECK((translation.segments[index].feedback & BOXRIG_COMPOSITION_FEEDBACK_REVERSE) != 0);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_foreground_new(100, 100, 100), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_underline_new(PANGO_UNDERLINE_NONE), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_weight_new(PANGO_WEIGHT_NORMAL), 0, G_MAXUINT);
    CHECK(boxrig_gtk4_input_translate_preedit("abc", attributes, 1, &translation));
    CHECK(translation.caret_offset == 1 && translation.selection_length == 0 && translation.segment_count == 0);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);
    /* Attribute indices are scalar boundaries, independent of grapheme layout. */
    attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 1, 3);
    CHECK(boxrig_gtk4_input_translate_preedit("é語", attributes, 1, &translation));
    CHECK(translation.caret_offset == 1 && translation.selection_offset == 1 && translation.selection_length == 2);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    char many[1025];
    memset(many, 'a', sizeof(many) - 1); many[sizeof(many) - 1] = '\0';
    attributes = pango_attr_list_new();
    for (guint offset = 0; offset + 1 < sizeof(many); offset += 2)
        add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), offset, offset + 1);
    CHECK(boxrig_gtk4_input_translate_preedit(many, attributes, 10, &translation));
    CHECK(translation.caret_offset == 10 && translation.selection_length == 0 && translation.segment_count == 0);
    boxrig_gtk4_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);
    return true;
}

static bool test_cairo_font_cache_lifetime(void)
{
    /* Native GTK text uses the default font map, which can outlive its widgets.
     * Exercise it even when the window system does not draw GTK decorations. */
    GtkWidget *label = gtk_label_new("GTK 4 font cache lifetime");
    CHECK(label != NULL);
    g_object_ref_sink(label);
    PangoLayout *layout = gtk_widget_create_pango_layout(label, "GTK 4 font cache lifetime");
    CHECK(layout != NULL);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 48);
    CHECK(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    cairo_t *cairo = cairo_create(surface);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    pango_cairo_show_layout(cairo, layout);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    g_object_unref(layout);
    g_object_unref(label);
    printf("PASS GTK 4 Pango layout releases caller-owned objects before Cairo reset\n");
    return true;
}

int main(int argc, char **argv)
{
    bool passed;

    if (argc != 2) {
        fprintf(stderr, "usage: test_gtk4_widget FONT_FILE\n");
        return 2;
    }
    if (!gtk_init_check()) {
        fprintf(stderr, "FAIL GTK initialization\n");
        return 1;
    }
    if (getenv("BOXRIG_TEST_SCROLLBAR_ONLY") != NULL) {
        bool passed = test_standard_scrollbar_visibility() && test_scrollbar_visibility_during_scale_change() &&
            test_cairo_font_cache_lifetime();
        while (g_main_context_pending(NULL)) (void)g_main_context_iteration(NULL, false);
        fflush(stdout);
        /* Native GTK text caches outlive widgets and must precede Cairo cleanup. */
        PangoFontMap *font_map = pango_cairo_font_map_get_default();
        if (PANGO_IS_FC_FONT_MAP(font_map)) {
            pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(font_map));
        }
        pango_cairo_font_map_set_default(NULL);
        cairo_debug_reset_static_data();
        FcFini();
        return passed ? 0 : 1;
    }
    passed = getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL
        ? test_widget_frontend_component_fixture(argv[1]) && test_scrollbar_visibility_during_scale_change() &&
            test_cairo_font_cache_lifetime()
        : test_standard_scrollbar_visibility() && test_scrollbar_visibility_during_scale_change() &&
             test_widget_frontend_component_fixture(argv[1]) &&
             test_preedit_clause_attributes() && test_widget_preedit_translation() &&
             test_widget_lifecycle_layout_and_frame(argv[1]) &&
             test_sample_container_reserves_scrollbar_outside_grid() &&
             test_constrained_zoom_uses_one_layout_action() &&
             test_widget_zoom_commits_complete_resize_presentation() &&
             test_widget_resize_preview_covers_expanded_allocation() &&
             test_widget_input_lifecycle_releases_context() &&
             test_widget_input_fifo_and_backpressure() &&
             test_widget_clipboard_actions() && test_cairo_font_cache_lifetime();
    fflush(stdout);
    /* Native GTK text caches outlive widgets and must precede Cairo cleanup. */
    PangoFontMap *font_map = pango_cairo_font_map_get_default();
    if (PANGO_IS_FC_FONT_MAP(font_map)) {
        pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(font_map));
    }
    pango_cairo_font_map_set_default(NULL);
    cairo_debug_reset_static_data();
    FcFini();
    if (passed) {
        printf("PASS GTK 4 widget lifecycle layout input frame and snapshot\n");
    }
    return passed ? 0 : 1;
}
