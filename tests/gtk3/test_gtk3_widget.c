/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/gtk3.h"
#include "../../src/gtk3/input.h"
#include "../../src/core/terminal_internal.h"
#include "../composition_fixture.h"
#include "../frontend_component_fixture.h"
#include "../../examples/gtk3/external_scrollbar.h"
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

static int failing_surface_width;
static int failing_surface_height;
static unsigned int failing_surface_after;
static unsigned int matching_surface_calls;

/** @copydoc cairo_image_surface_create */
cairo_surface_t *__real_cairo_image_surface_create(
    cairo_format_t format, int width, int height);

/**
 * Injects a real Cairo error surface during native allocation preparation.
 * @param format Requested image format.
 * @param width Requested pixel width.
 * @param height Requested pixel height.
 * @return The real image, or Cairo's nil surface while the fault is armed.
 */
cairo_surface_t *__wrap_cairo_image_surface_create(
    cairo_format_t format, int width, int height)
{
    if (failing_surface_after != 0 && width == failing_surface_width &&
        height == failing_surface_height &&
        ++matching_surface_calls >= failing_surface_after) {
        return __real_cairo_image_surface_create(CAIRO_FORMAT_INVALID, width, height);
    }
    return __real_cairo_image_surface_create(format, width, height);
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

static bool take_grid_resize_action(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_grid_resize *resize_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_gtk3_terminal_widget_take_layout_action(widget, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        return false;
    }
    resize_out->revision = action.revision;
    resize_out->rows = action.value.grid_resize.rows;
    resize_out->columns = action.value.grid_resize.columns;
    return true;
}

static bool draw_widget(GtkWidget *widget)
{
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    cairo_surface_t *surface;
    cairo_t *cairo;
    bool succeeded;
    gboolean handled = FALSE;

    CHECK(width > 0 && height > 0);
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    CHECK(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    cairo = cairo_create(surface);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    g_signal_emit_by_name(widget, "draw", cairo, &handled);
    succeeded = cairo_status(cairo) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    (void)handled;
    CHECK(succeeded);
    return true;
}

static bool draw_widget_pixel(
    GtkWidget *widget,
    int x,
    int y,
    uint32_t *pixel_out)
{
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    cairo_surface_t *surface;
    cairo_t *cairo;
    unsigned char *pixels;
    int stride;

    CHECK(width > 0 && height > 0 && x >= 0 && x < width && y >= 0 &&
          y < height && pixel_out != NULL);
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    CHECK(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    cairo = cairo_create(surface);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    gtk_widget_draw(widget, cairo);
    CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    pixels = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    CHECK(pixels != NULL && stride >= width * 4);
    memcpy(pixel_out, pixels + (size_t)y * (size_t)stride + (size_t)x * 4,
           sizeof(*pixel_out));
    cairo_surface_destroy(surface);
    return true;
}

static bool accessible_has_state(
    AtkObject *accessible,
    AtkStateType state)
{
    AtkStateSet *states = atk_object_ref_state_set(accessible);
    bool present;

    CHECK(states != NULL);
    present = atk_state_set_contains_state(states, state);
    g_object_unref(states);
    return present;
}

typedef struct gtk3_fixture_host {
    GtkWindow *window;
    BoxrigGtk3TerminalWidget *attached_widget;
} gtk3_fixture_host;

typedef struct gtk3_fixture_component {
    gtk3_fixture_host *host;
    BoxrigGtk3TerminalWidget *widget;
} gtk3_fixture_component;

typedef struct clipboard_read_result {
    char *text;
    bool completed;
} clipboard_read_result;

typedef struct mouse_route_observer {
    boxrig_mouse_event events[4];
    boxrig_ui_hit_test_result hits[4];
    size_t count;
} mouse_route_observer;

static boxrig_input_mouse_route capture_mouse_route(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    mouse_route_observer *observer = context;

    if (hit == NULL || observer->count >= 4) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    observer->events[observer->count] = *event;
    observer->hits[observer->count++] = *hit;
    return event->action == BOXRIG_MOUSE_PRESS &&
                   event->button == BOXRIG_MOUSE_BUTTON_LEFT
               ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
               : BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static void clipboard_text_read(
    GtkClipboard *clipboard,
    const gchar *text,
    gpointer user_data)
{
    clipboard_read_result *result = user_data;

    (void)clipboard;
    result->text = text == NULL ? NULL : g_strdup(text);
    result->completed = true;
}

static void *gtk3_fixture_create_host(void)
{
    gtk3_fixture_host *host = calloc(1, sizeof(*host));

    if (host == NULL) {
        return NULL;
    }
    host->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    if (host->window == NULL) {
        free(host);
        return NULL;
    }
    g_object_ref_sink(host->window);
    gtk_window_set_decorated(host->window, false);
    return host;
}

static void gtk3_fixture_destroy_host(void *opaque_host)
{
    gtk3_fixture_host *host = opaque_host;

    if (host == NULL) {
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(host->window));
    g_object_unref(host->window);
    free(host);
}

static boxrig_ui_status gtk3_fixture_create_component(
    void *opaque_host,
    boxrig_ui_model *model,
    boxrig_ui_component_attachment attachment,
    void **component_out)
{
    gtk3_fixture_host *host = opaque_host;
    boxrig_gtk3_terminal_widget_config config =
        boxrig_gtk3_terminal_widget_config_default();
    gtk3_fixture_component *component;
    BoxrigGtk3TerminalWidget *widget = NULL;
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
    status = boxrig_gtk3_terminal_widget_create(model, &config, &widget);
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
        gtk_container_add(GTK_CONTAINER(host->window), GTK_WIDGET(widget));
        host->attached_widget = widget;
    }
    *component_out = component;
    return BOXRIG_UI_OK;
}

static void gtk3_fixture_destroy_component(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    if (component == NULL) {
        return;
    }
    (void)boxrig_gtk3_terminal_widget_quiesce(component->widget);
    if (component->host->attached_widget == component->widget) {
        gtk_container_remove(
            GTK_CONTAINER(component->host->window),
            GTK_WIDGET(component->widget));
        component->host->attached_widget = NULL;
    }
    g_object_unref(component->widget);
    free(component);
}

static boxrig_ui_status gtk3_fixture_quiesce_component(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_quiesce(component->widget);
}

static boxrig_ui_view *gtk3_fixture_component_view(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_view(component->widget);
}

static bool gtk3_fixture_get_effective_metrics(
    void *opaque_component,
    boxrig_font_effective_metrics *metrics_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_effective_metrics(
               component->widget,
               metrics_out) == BOXRIG_FONT_CATALOG_OK;
}

static boxrig_ui_status gtk3_fixture_allocate(
    void *opaque_component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out)
{
    gtk3_fixture_component *component = opaque_component;
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
    status = boxrig_gtk3_terminal_widget_set_window_state(
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
    gtk_widget_show_all(GTK_WIDGET(component->host->window));
    while (boxrig_gtk3_terminal_widget_get_layout(
               component->widget,
               layout_out) == BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status gtk3_fixture_set_preferred_grid(
    void *opaque_component,
    size_t rows,
    size_t columns)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_preferred_grid(
        component->widget,
        rows,
        columns);
}

static bool gtk3_fixture_get_preferred_grid(
    void *opaque_component,
    size_t *rows_out,
    size_t *columns_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_preferred_grid(
               component->widget,
               rows_out,
               columns_out) == BOXRIG_UI_OK;
}

static boxrig_ui_size_command_state gtk3_fixture_get_size_command_state(
    void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;
    boxrig_ui_size_command_state state = BOXRIG_UI_SIZE_COMMAND_IDLE;

    (void)boxrig_gtk3_terminal_widget_get_size_command_state(
        component->widget,
        &state);
    return state;
}

static bool gtk3_fixture_take_layout_action(
    void *opaque_component,
    boxrig_ui_layout_action *action_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_take_layout_action(
        component->widget,
        action_out);
}

static boxrig_ui_status gtk3_fixture_complete_size_command(
    void *opaque_component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_complete_size_command(
        component->widget,
        revision,
        outcome,
        allocation_width,
        allocation_height,
        applied_out,
        layout_out);
}

static boxrig_ui_status gtk3_fixture_complete_grid_resize(
    void *opaque_component,
    uint64_t revision,
    bool succeeded)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_complete_grid_resize(
        component->widget, revision, succeeded);
}

static boxrig_ui_status gtk3_fixture_set_visible(
    void *opaque_component,
    bool visible)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_visible(
        component->widget,
        visible);
}

static boxrig_ui_status gtk3_fixture_set_frame_request_callback(
    void *opaque_component,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_frame_request_callback(
        component->widget,
        callback,
        context);
}

static boxrig_ui_status gtk3_fixture_frame(
    void *opaque_component,
    boxrig_ui_component_frame_result *result_out)
{
    gtk3_fixture_component *component = opaque_component;

    boxrig_ui_status status = boxrig_gtk3_terminal_widget_frame(component->widget, result_out);
    if (status == BOXRIG_UI_OK && getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL) {
        /* Drive the toolkit's queued GLArea render before comparing the shared
         * synchronous fixture's completed-frame contract. */
        if (!draw_widget(GTK_WIDGET(component->widget))) return BOXRIG_UI_UNAVAILABLE;
        status = boxrig_gtk3_terminal_widget_frame(component->widget, result_out);
        boxrig_gtk_render_state state;
        if (status == BOXRIG_UI_OK &&
            (boxrig_gtk3_terminal_widget_get_render_state(component->widget, &state) != BOXRIG_UI_OK ||
             state.active_backend != BOXRIG_GTK_RENDER_BACKEND_OPENGL)) return BOXRIG_UI_UNAVAILABLE;
    }
    return status;
}

static boxrig_ui_status gtk3_fixture_queue_paste(
    void *opaque_component,
    const uint8_t *bytes,
    size_t length)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_queue_paste(
        component->widget,
        bytes,
        length);
}

static bool gtk3_fixture_run_native_input_transcript(
    void *opaque_component,
    const boxrig_test_frontend_input_script *script,
    boxrig_test_frontend_input_observation *observation_out)
{
    gtk3_fixture_component *component = opaque_component;
    GtkWidget *widget;
    GdkEventFocus focus_event;
    GdkEventKey key_event;
    GdkEventButton button_event;
    GdkEventMotion motion_event;
    boxrig_ui_component_frame_result frame_result;
    boxrig_composition_view composition_view;
    gboolean handled = FALSE;
    unsigned int scale;
    double x;
    double y;
    bool passed = false;

    if (component == NULL || script == NULL || observation_out == NULL) {
        return false;
    }
    /* A mapped GLArea can precede the asynchronous Wayland registry reply.
     * Wait for input routing before this synchronous native-event transcript. */
    boxrig_gtk_ime_state ime_state;
    for (;;) {
        CHECK(boxrig_gtk3_terminal_widget_frame(
                  component->widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_get_ime_state(
                  component->widget, &ime_state) == BOXRIG_UI_OK);
        if (ime_state.active_route != BOXRIG_GTK_IME_ROUTE_PENDING) break;
        (void)g_main_context_iteration(NULL, true);
    }
    widget = GTK_WIDGET(component->widget);
    scale = (unsigned int)gtk_widget_get_scale_factor(widget);
    if (scale == 0 ||
        boxrig_gtk3_terminal_widget_set_key_router(
            component->widget,
            boxrig_test_frontend_capture_key,
            observation_out) != BOXRIG_UI_OK ||
        boxrig_gtk3_terminal_widget_set_mouse_router(
            component->widget,
            boxrig_test_frontend_capture_mouse,
            observation_out) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    x = (double)script->x /
        ((double)BOXRIG_UI_FIXED_ONE * (double)scale);
    y = (double)script->y /
        ((double)BOXRIG_UI_FIXED_ONE * (double)scale);

    memset(&focus_event, 0, sizeof(focus_event));
    focus_event.type = GDK_FOCUS_CHANGE;
    focus_event.window = gtk_widget_get_window(widget);
    focus_event.send_event = TRUE;
    focus_event.in = TRUE;
    g_signal_emit_by_name(widget, "focus-in-event", &focus_event, &handled);
    CHECK(boxrig_ui_view_focused(boxrig_gtk3_terminal_widget_view(component->widget)));
    if (boxrig_gtk3_terminal_widget_frame(
            component->widget,
            &frame_result) != BOXRIG_UI_OK ||
        boxrig_composition_get_view(
            boxrig_gtk3_terminal_widget_composition(component->widget),
            &composition_view) != BOXRIG_COMPOSITION_OK) {
        goto cleanup;
    }
    observation_out->focused_after_enter = composition_view.focused;

    memset(&key_event, 0, sizeof(key_event));
    key_event.type = GDK_KEY_PRESS;
    key_event.window = gtk_widget_get_window(widget);
    key_event.send_event = TRUE;
    key_event.time = 1;
    key_event.state = 0;
    key_event.keyval = GDK_KEY_F1;
    key_event.hardware_keycode = 67;
    key_event.group = 0;
    g_signal_emit_by_name(widget, "key-press-event", &key_event, &handled);
    if (!handled) {
        goto cleanup;
    }
    handled = FALSE;
    key_event.time = 2;
    g_signal_emit_by_name(widget, "key-press-event", &key_event, &handled);
    if (!handled) {
        goto cleanup;
    }
    handled = FALSE;
    key_event.type = GDK_KEY_RELEASE;
    key_event.time = 3;
    g_signal_emit_by_name(widget, "key-release-event", &key_event, &handled);
    if (!handled) {
        goto cleanup;
    }

    memset(&button_event, 0, sizeof(button_event));
    button_event.type = GDK_BUTTON_PRESS;
    button_event.window = gtk_widget_get_window(widget);
    button_event.send_event = TRUE;
    button_event.time = 10;
    button_event.x = x;
    button_event.y = y;
    button_event.state = GDK_CONTROL_MASK;
    button_event.button = 1;
    handled = FALSE;
    g_signal_emit_by_name(
        widget,
        "button-press-event",
        &button_event,
        &handled);
    if (!handled) {
        goto cleanup;
    }
    memset(&motion_event, 0, sizeof(motion_event));
    motion_event.type = GDK_MOTION_NOTIFY;
    motion_event.window = gtk_widget_get_window(widget);
    motion_event.send_event = TRUE;
    motion_event.time = 11;
    motion_event.x = x;
    motion_event.y = y;
    motion_event.state = GDK_SHIFT_MASK | GDK_BUTTON1_MASK;
    handled = FALSE;
    g_signal_emit_by_name(
        widget,
        "motion-notify-event",
        &motion_event,
        &handled);
    if (!handled) {
        goto cleanup;
    }
    button_event.type = GDK_BUTTON_RELEASE;
    button_event.time = 12;
    button_event.state = GDK_BUTTON1_MASK;
    handled = FALSE;
    g_signal_emit_by_name(
        widget,
        "button-release-event",
        &button_event,
        &handled);
    if (!handled ||
        boxrig_gtk3_terminal_widget_frame(
            component->widget,
            &frame_result) != BOXRIG_UI_OK) {
        goto cleanup;
    }

    handled = FALSE;
    focus_event.in = FALSE;
    g_signal_emit_by_name(widget, "focus-out-event", &focus_event, &handled);
    CHECK(!boxrig_ui_view_focused(boxrig_gtk3_terminal_widget_view(component->widget)));
    if (boxrig_gtk3_terminal_widget_frame(
            component->widget,
            &frame_result) != BOXRIG_UI_OK ||
        boxrig_composition_get_view(
            boxrig_gtk3_terminal_widget_composition(component->widget),
            &composition_view) != BOXRIG_COMPOSITION_OK) {
        goto cleanup;
    }
    observation_out->focused_after_leave = composition_view.focused;
    passed = true;

cleanup:
    if (boxrig_gtk3_terminal_widget_set_key_router(
            component->widget,
            NULL,
            NULL) != BOXRIG_UI_OK ||
        boxrig_gtk3_terminal_widget_set_mouse_router(
            component->widget,
            NULL,
            NULL) != BOXRIG_UI_OK) {
        passed = false;
    }
    return passed;
}

static boxrig_font_catalog_status gtk3_fixture_register_font_file(
    void *opaque_component,
    const char *path,
    size_t path_length,
    boxrig_font_resource_id *resource_id_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_register_font_file(
        component->widget,
        path,
        path_length,
        0,
        resource_id_out);
}

static boxrig_ui_status gtk3_fixture_set_font_specification(
    void *opaque_component,
    const boxrig_font_specification *specification)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_font_specification(
        component->widget,
        specification);
}

static boxrig_ui_status gtk3_fixture_get_font_specification(
    void *opaque_component,
    boxrig_font_specification *specification_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_font_specification(
        component->widget,
        specification_out);
}

static boxrig_font_resource_id gtk3_fixture_primary_font_resource(
    void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_primary_font_resource(
        component->widget);
}

static uint64_t gtk3_fixture_font_catalog_revision(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_font_catalog_revision(
        component->widget);
}

static boxrig_ui_status gtk3_fixture_set_line_height(
    void *opaque_component,
    float adjustment_dip)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_line_height_adjustment(
        component->widget,
        adjustment_dip);
}

static boxrig_ui_status gtk3_fixture_get_line_height(
    void *opaque_component,
    float *adjustment_dip_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_line_height_adjustment(
        component->widget,
        adjustment_dip_out);
}

static boxrig_ui_status gtk3_fixture_visual_zoom(
    void *opaque_component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_visual_zoom(
        component->widget,
        direction,
        policy,
        changed_out);
}

static boxrig_ui_status gtk3_fixture_visual_zoom_reset(
    void *opaque_component,
    bool *changed_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_visual_zoom_reset(
        component->widget,
        changed_out);
}

static boxrig_ui_status gtk3_fixture_set_text_direction(
    void *opaque_component,
    boxrig_text_direction_policy policy)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_text_direction(
        component->widget,
        policy);
}

static boxrig_text_direction_policy gtk3_fixture_text_direction(
    void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_text_direction(component->widget);
}

static bool gtk3_fixture_copy_selection(
    void *opaque_host,
    void *opaque_component,
    const uint8_t *expected,
    size_t expected_length)
{
    gtk3_fixture_component *component = opaque_component;
    boxrig_ui_view_action action;
    boxrig_ui_view_action_capabilities capabilities;
    clipboard_read_result result;
    GtkClipboard *clipboard;
    bool matched;

    (void)opaque_host;
    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    capabilities = boxrig_gtk3_terminal_widget_view_action_capabilities(
        component->widget);
    if ((capabilities.copy_targets &
         BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD) == 0) {
        return boxrig_gtk3_terminal_widget_apply_view_action(
                   component->widget,
                   &action) == BOXRIG_UI_UNAVAILABLE;
    }
    if (boxrig_gtk3_terminal_widget_apply_view_action(
            component->widget,
            &action) != BOXRIG_UI_OK) {
        return false;
    }
    clipboard = gtk_clipboard_get_for_display(
        gtk_widget_get_display(GTK_WIDGET(component->widget)),
        GDK_SELECTION_CLIPBOARD);
    memset(&result, 0, sizeof(result));
    gtk_clipboard_request_text(clipboard, clipboard_text_read, &result);
    while (!result.completed) {
        (void)g_main_context_iteration(NULL, true);
    }
    matched = result.text != NULL && strlen(result.text) == expected_length &&
              memcmp(result.text, expected, expected_length) == 0;
    g_free(result.text);
    return matched;
}

static boxrig_ui_status gtk3_fixture_set_transparency(
    void *opaque_component,
    const boxrig_ui_transparency_config *config)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_set_transparency(
        component->widget,
        config);
}

static boxrig_ui_status gtk3_fixture_get_transparency(
    void *opaque_component,
    boxrig_ui_transparency_state *state_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_transparency(
        component->widget,
        state_out);
}

static uint32_t gtk3_fixture_capabilities(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_capabilities(component->widget);
}

static boxrig_ui_status gtk3_fixture_get_diagnostic(
    void *opaque_component,
    boxrig_ui_component_diagnostic *diagnostic_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_diagnostic(
        component->widget,
        diagnostic_out);
}

static boxrig_composition *gtk3_fixture_composition(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_composition(component->widget);
}

static bool gtk3_fixture_observe_presentation(
    void *opaque_component,
    const boxrig_test_frontend_presentation_probe *probe,
    boxrig_test_frontend_presentation_observation *observation_out)
{
    gtk3_fixture_component *component = opaque_component;
    boxrig_test_frontend_presentation_probe logical_probe;
    GtkWidget *widget;
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
            boxrig_gtk3_terminal_widget_composition(component->widget),
            &composition) == BOXRIG_COMPOSITION_OK);
        if (composition.candidate.visible) {
            GtkAllocation allocation;
            gtk_widget_get_allocation(widget, &allocation);
            CHECK(allocation.x == 31 && allocation.y == 17);
            CHECK(candidate_location_count != 0);
            CHECK(last_candidate_rectangle.x == allocation.x + (int)(composition.candidate.spot_x / scale));
            CHECK(last_candidate_rectangle.y == allocation.y + (int)(composition.candidate.exclude_y / scale));
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
    while (g_main_context_pending(NULL)) {
        (void)g_main_context_iteration(NULL, false);
    }
    width = gtk_widget_get_allocated_width(widget);
    height = gtk_widget_get_allocated_height(widget);
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
        return false;
    }
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }
    cairo = cairo_create(surface);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cairo);
        cairo_surface_destroy(surface);
        return false;
    }
    gtk_widget_draw(widget, cairo);
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
        boxrig_gtk3_terminal_widget_presentation_generation(
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
gtk3_fixture_view_action_capabilities(void *opaque_component)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_view_action_capabilities(
        component->widget);
}

static boxrig_ui_status gtk3_fixture_apply_view_action(
    void *opaque_component,
    const boxrig_ui_view_action *action)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_apply_view_action(
        component->widget,
        action);
}

static bool gtk3_fixture_get_scrollbar_state(void *component, boxrig_scrollbar_state *state_out)
{
    gtk3_fixture_component *fixture = component;
    example_external_scrollbar bar = {.component = fixture->widget};
    return check_external_scrollbar_example(&bar, state_out);
}

static bool gtk3_fixture_get_scrollbar_model(
    void *opaque_component,
    boxrig_scrollbar_model *model_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_get_scrollbar_model(
               component->widget,
               model_out) == BOXRIG_UI_OK;
}

static boxrig_ui_status gtk3_fixture_apply_scrollbar_action(
    void *opaque_component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    gtk3_fixture_component *component = opaque_component;

    return boxrig_gtk3_terminal_widget_apply_scrollbar_action(
        component->widget,
        action,
        position,
        changed_out);
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

static bool test_preedit_translation(void)
{
    boxrig_gtk3_preedit_translation translation;
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
    CHECK(boxrig_gtk3_input_translate_preedit(
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
    CHECK(translation.caret_offset == boxrig_composition_fixture.caret_offset);
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    attributes = pango_attr_list_new();
    CHECK(attributes != NULL);
    add_preedit_attribute(
        attributes,
        pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
        4,
        5);
    CHECK(boxrig_gtk3_input_translate_preedit(
        boxrig_composition_fixture.utf8,
        attributes,
        2,
        &translation));
    /* The end of ASCII e is a valid scalar boundary inside e + combining acute. */
    CHECK(translation.segment_count == 1);
    CHECK(translation.segments[0].text_offset == 4 && translation.segments[0].text_length == 1);
    CHECK(translation.segments[0].feedback == BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);
    return true;
}

static bool wait_for_layout(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_layout_result *layout_out)
{
    while (boxrig_gtk3_terminal_widget_get_layout(widget, layout_out) ==
           BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    return true;
}

static void count_frame_request(void *context)
{
    size_t *count = context;

    (*count)++;
}

static bool test_pointer_router_retains_fractional_hit(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_hit_test_result expected_hit;
    mouse_route_observer observer;
    GdkEventButton event;
    gboolean handled = FALSE;
    const uint8_t *output = NULL;
    double native_x;
    double native_y;
    unsigned int scale;

    memset(&observer, 0, sizeof(observer));
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    CHECK(wait_for_layout(widget, &layout));
    CHECK(layout.rows == terminal_config.rows &&
          layout.columns == terminal_config.columns);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale != 0);
    native_x = ((double)metrics.cell_width + 0.25) / (double)scale;
    native_y = 0.25 / (double)scale;
    CHECK(boxrig_gtk3_terminal_widget_hit_test(
              widget,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE + 16,
              16,
              &expected_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_set_mouse_router(
              widget,
              capture_mouse_route,
              &observer) == BOXRIG_UI_OK);

    memset(&event, 0, sizeof(event));
    event.type = GDK_BUTTON_PRESS;
    event.button = 1;
    event.x = native_x;
    event.y = native_y;
    event.state = GDK_CONTROL_MASK;
    g_signal_emit_by_name(
        widget,
        "button-press-event",
        &event,
        &handled);
    CHECK(handled);
    handled = FALSE;
    event.type = GDK_BUTTON_RELEASE;
    event.state = GDK_BUTTON1_MASK | GDK_SHIFT_MASK;
    g_signal_emit_by_name(
        widget,
        "button-release-event",
        &event,
        &handled);
    CHECK(handled);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(observer.count == 2);
    CHECK(observer.events[0].action == BOXRIG_MOUSE_PRESS &&
          observer.events[1].action == BOXRIG_MOUSE_RELEASE);
    CHECK(observer.events[0].modifiers == BOXRIG_MODIFIER_CONTROL);
    CHECK(observer.events[1].modifiers == BOXRIG_MODIFIER_SHIFT);
    CHECK(observer.events[0].x == (int64_t)metrics.cell_width &&
          observer.events[0].y == 0);
    CHECK(observer.hits[0].x ==
          (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE + 16);
    CHECK(observer.hits[0].y == 16);
    CHECK(observer.hits[0].logical_column == expected_hit.logical_column);
    CHECK(observer.hits[0].layout_generation ==
          expected_hit.layout_generation);
    CHECK(observer.hits[0].annotation_generation ==
          expected_hit.annotation_generation);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    /* Fractions outside the top-left edge must not round into the grid. */
    observer.count = 0;
    event.x = -0.001 / scale;
    event.y = -0.5 / scale;
    event.state = 0;
    event.type = GDK_BUTTON_PRESS;
    g_signal_emit_by_name(widget, "button-press-event", &event, &handled);
    event.type = GDK_BUTTON_RELEASE;
    g_signal_emit_by_name(widget, "button-release-event", &event, &handled);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(observer.count == 2);
    CHECK(observer.events[0].x == -1 && observer.events[0].y == -1);
    CHECK(!observer.hits[0].inside_grid && observer.hits[0].x < 0);

    CHECK(boxrig_gtk3_terminal_widget_set_mouse_router(widget, NULL, NULL) ==
          BOXRIG_UI_OK);
    {
        const char mode[] = "\033[?1000;1016h";
        CHECK(feed_bytes(terminal, (const uint8_t *)mode, sizeof(mode) - 1));
        event.x = native_x;
        event.y = native_y;
        event.type = GDK_BUTTON_PRESS;
        g_signal_emit_by_name(widget, "button-press-event", &event, &handled);
        event.type = GDK_BUTTON_RELEASE;
        g_signal_emit_by_name(widget, "button-release-event", &event, &handled);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        char report[96];
        int length = snprintf(report, sizeof(report), "\033[<0;%u;1M\033[<0;%u;1m",
            metrics.cell_width + 1, metrics.cell_width + 1);
        CHECK(length > 0 && boxrig_terminal_output_peek(terminal, &output) == (size_t)length);
        CHECK(memcmp(output, report, (size_t)length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, (size_t)length) == BOXRIG_STATUS_OK);
    }
    {
        static const uint8_t pending[] = "\033[?1000;1006h\033[?2026hX";
        static const uint8_t report[] = "\033[<65;2;1M";
        static const uint8_t finish[] = "\033[?2026l";
        GdkEventScroll scroll = {0};
        size_t requests = 0;

        /* A pending synchronized image is not a scheduled input dispatch. */
        CHECK(feed_bytes(terminal, pending, sizeof(pending) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
        CHECK(boxrig_gtk3_terminal_widget_set_frame_request_callback(
                  widget, count_frame_request, &requests) == BOXRIG_UI_OK);
        scroll.type = GDK_SCROLL;
        scroll.direction = GDK_SCROLL_DOWN;
        scroll.x = native_x;
        scroll.y = native_y;
        handled = FALSE;
        g_signal_emit_by_name(widget, "scroll-event", &scroll, &handled);
        CHECK(handled && requests != 0);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(report) - 1);
        CHECK(memcmp(output, report, sizeof(report) - 1) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, sizeof(report) - 1) == BOXRIG_STATUS_OK);
        CHECK(feed_bytes(terminal, finish, sizeof(finish) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(!frame_result.frame_pending);
        CHECK(boxrig_gtk3_terminal_widget_set_frame_request_callback(
                  widget, NULL, NULL) == BOXRIG_UI_OK);
    }
    gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_destroy(GTK_WIDGET(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_resize_preview_covers_expanded_allocation(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window = NULL;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    GtkAllocation allocation;
    uint32_t expanded_pixel = 0;
    bool shares_parent_drawable;

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    CHECK(wait_for_layout(widget, &layout));
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    shares_parent_drawable =
        !gtk_widget_get_has_window(GTK_WIDGET(widget)) &&
        gtk_widget_get_window(GTK_WIDGET(widget)) ==
            gtk_widget_get_parent_window(GTK_WIDGET(widget));

    gtk_widget_get_allocation(GTK_WIDGET(widget), &allocation);
    CHECK(allocation.width > 0 && allocation.height > 0);
    allocation.width += 23;
    allocation.height += 17;
    gtk_widget_size_allocate(GTK_WIDGET(widget), &allocation);
    CHECK(draw_widget_pixel(
        GTK_WIDGET(widget),
        allocation.width - 1,
        allocation.height - 1,
        &expanded_pixel));
    CHECK((expanded_pixel >> 24) == 255);

    gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_destroy(GTK_WIDGET(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(shares_parent_drawable);
    return true;
}

typedef struct resize_boundary_observer {
    BoxrigGtk3TerminalWidget *widget;
    int width;
    int height;
    unsigned int allocations;
    unsigned int draws;
    unsigned int wakeups;
    unsigned int allocation_wakeups;
    bool inspect_before_draw;
    bool early_resize;
    bool offscreen_draw_succeeded;
    bool frame_replaced_preview;
    boxrig_ui_status waiting_frame_status;
    boxrig_ui_component_frame_result waiting_frame;
    bool expired;
} resize_boundary_observer;

static void resize_boundary_requested(void *context)
{
    resize_boundary_observer *observer = context;

    observer->wakeups++;
}

static void resize_boundary_allocated(
    GtkWidget *widget,
    GtkAllocation *allocation,
    gpointer user_data)
{
    resize_boundary_observer *observer = user_data;
    boxrig_ui_grid_resize resize;

    if (allocation->width != observer->width ||
        allocation->height != observer->height) {
        return;
    }
    observer->allocations++;
    observer->allocation_wakeups = observer->wakeups;
    if (observer->inspect_before_draw && observer->allocations == 1) {
        uint64_t generation =
            boxrig_gtk3_terminal_widget_presentation_generation(observer->widget);

        observer->early_resize =
            take_grid_resize_action(
                observer->widget, &resize);
        observer->offscreen_draw_succeeded = draw_widget(widget);
        observer->early_resize = observer->early_resize ||
            take_grid_resize_action(
                observer->widget, &resize);
        observer->waiting_frame_status = boxrig_gtk3_terminal_widget_frame(
            observer->widget, &observer->waiting_frame);
        observer->frame_replaced_preview =
            boxrig_gtk3_terminal_widget_presentation_generation(observer->widget) !=
            generation;
    }
}

static gboolean resize_boundary_drawn(
    GtkWidget *widget,
    cairo_t *cairo,
    gpointer user_data)
{
    resize_boundary_observer *observer = user_data;

    if (gdk_cairo_get_drawing_context(cairo) != NULL &&
        gtk_widget_get_allocated_width(widget) == observer->width &&
        gtk_widget_get_allocated_height(widget) == observer->height) {
        observer->draws++;
    }
    return FALSE;
}

static gboolean resize_boundary_expired(gpointer user_data)
{
    resize_boundary_observer *observer = user_data;

    observer->expired = true;
    return G_SOURCE_REMOVE;
}

static bool test_grid_resize_tracks_native_allocation_presentation(void)
{
    static const uint8_t first_color[] = "\033[48;2;80;96;112m\033[2J";
    static const uint8_t second_color[] = "\033[48;2;112;96;80m\033[2J";
    static const uint8_t tracking[] = "\033[?1000h\033[?1006h";
    static const uint8_t mouse_output[] = "\033[<0;8;4M\033[<0;8;4m";
    static const size_t later_rows[] = {8, 4, 4, 6, 8, 6, 4};
    static const size_t later_columns[] = {16, 8, 8, 12, 16, 12, 8};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window = NULL;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_layout_action layout_action;
    boxrig_ui_grid_resize resize;
    boxrig_ui_grid_resize in_flight;
    boxrig_ui_component_diagnostic diagnostic;
    resize_boundary_observer observer = {0};
    uint64_t previous_revision = 0;
    uint64_t generation;
    const uint8_t *output;
    uint32_t old_pixel;
    uint32_t pixel;
    size_t rows;
    size_t columns;
    unsigned int wakeups;
    guint guard;
    size_t index;
    int scale;

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    widget_config.frame_request_callback = resize_boundary_requested;
    widget_config.frame_request_context = &observer;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(model, &widget_config, &widget) ==
          BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    observer.widget = widget;
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) ==
          BOXRIG_FONT_CATALOG_OK);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale > 0);
    gtk_window_set_default_size(window, (int)metrics.cell_width * 6 / scale,
                                (int)metrics.cell_height * 3 / scale);
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    CHECK(wait_for_layout(widget, &layout));
    CHECK(!boxrig_gtk3_terminal_widget_has_presentation(widget));
    CHECK(!take_grid_resize_action(widget, &resize));
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) ==
          BOXRIG_FONT_CATALOG_OK);
    g_signal_connect_after(widget, "size-allocate",
                           G_CALLBACK(resize_boundary_allocated), &observer);
    g_signal_connect_after(widget, "draw",
                           G_CALLBACK(resize_boundary_drawn), &observer);

    observer.width = gtk_widget_get_allocated_width(GTK_WIDGET(widget));
    observer.height = gtk_widget_get_allocated_height(GTK_WIDGET(widget));
    guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
    while (!observer.expired && observer.draws == 0) {
        (void)g_main_context_iteration(NULL, true);
    }
    if (!observer.expired) {
        g_source_remove(guard);
    }
    CHECK(!observer.expired);

    /* The first safe image uses the old core grid, so a different initial
     * allocation can reach its native boundary without a circular wait. */
    CHECK(boxrig_gtk3_terminal_widget_take_layout_action(
        widget, &layout_action));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
    CHECK(layout_action.value.grid_resize.rows == 3 &&
          layout_action.value.grid_resize.columns == 6);
    CHECK(boxrig_terminal_resize(
              terminal,
              layout_action.value.grid_resize.rows,
              layout_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
              widget, layout_action.revision, true) == BOXRIG_UI_OK);
    previous_revision = layout_action.revision;
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(!frame_result.frame_pending);

    for (index = 0; index < 2; index++) {
        unsigned int draws_before_take;

        CHECK(draw_widget_pixel(GTK_WIDGET(widget), (int)metrics.cell_width / scale, 0,
                                &old_pixel));
        CHECK(feed_bytes(terminal, index == 0 ? first_color : second_color,
                         sizeof(first_color) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        observer.width = (int)metrics.cell_width * (int)(8 + index * 4) / scale;
        observer.height = (int)metrics.cell_height * (int)(4 + index * 2) / scale;
        observer.allocations = 0;
        observer.draws = 0;
        observer.inspect_before_draw = index == 0;
        observer.early_resize = false;
        observer.expired = false;
        guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
        gtk_window_resize(window, observer.width, observer.height);
        while (!observer.expired &&
               (observer.allocations == 0 || observer.draws == 0)) {
            (void)g_main_context_iteration(NULL, true);
        }
        if (!observer.expired) {
            g_source_remove(guard);
        }
        CHECK(!observer.expired);
        CHECK(!observer.early_resize);
        if (observer.inspect_before_draw) {
            CHECK(observer.offscreen_draw_succeeded);
            CHECK(observer.waiting_frame_status == BOXRIG_UI_OK);
            CHECK(!observer.frame_replaced_preview);
            CHECK(observer.waiting_frame.frame_pending);
            CHECK(!observer.waiting_frame.additional_dispatch);
        }
        CHECK(observer.wakeups > observer.allocation_wakeups);
        generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
              BOXRIG_UI_OK);
        CHECK(frame_result.frame_pending && frame_result.additional_dispatch);
        CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) == generation);
        CHECK(draw_widget_pixel(GTK_WIDGET(widget), (int)metrics.cell_width / scale, 0,
                                &pixel));
        CHECK(pixel == old_pixel);
        draws_before_take = observer.draws;
        CHECK(take_grid_resize_action(widget, &resize));
        CHECK(resize.rows == 4 + index * 2 && resize.columns == 8 + index * 4);
        CHECK(resize.revision > previous_revision);
        CHECK(!take_grid_resize_action(widget, &resize));
        CHECK(observer.draws == draws_before_take);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
        CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) == generation);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(widget, 0, true) ==
              BOXRIG_UI_INVALID_ARGUMENT);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                  widget, resize.revision + 1, false) == BOXRIG_UI_STALE);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                  widget, resize.revision, true) == BOXRIG_UI_INVALID_ARGUMENT);
        if (index == 0) {
            GdkEventButton event = {0};
            gboolean handled = FALSE;

            CHECK(feed_bytes(terminal, tracking, sizeof(tracking) - 1));
            CHECK(boxrig_gtk3_terminal_widget_queue_paste(
                      widget, (const uint8_t *)"p", 1) == BOXRIG_UI_OK);
            CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
            CHECK(output[0] == 'p');
            CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
            event.type = GDK_BUTTON_PRESS;
            event.button = 1;
            event.x = (double)metrics.cell_width * 7.5 / (double)scale;
            event.y = (double)metrics.cell_height * 3.5 / (double)scale;
            g_signal_emit_by_name(widget, "button-press-event", &event, &handled);
            CHECK(handled);
            event.type = GDK_BUTTON_RELEASE;
            event.state = GDK_BUTTON1_MASK;
            handled = FALSE;
            g_signal_emit_by_name(widget, "button-release-event", &event, &handled);
            CHECK(handled);
            CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
            CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
            CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        }
        previous_revision = resize.revision;
        CHECK(boxrig_terminal_resize(terminal, resize.rows, resize.columns) ==
              BOXRIG_STATUS_OK);
        wakeups = observer.wakeups;
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                  widget, resize.revision, true) == BOXRIG_UI_OK);
        CHECK(observer.wakeups > wakeups);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                  widget, resize.revision, true) == BOXRIG_UI_STALE);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
              BOXRIG_UI_OK);
        CHECK(!frame_result.frame_pending);
        CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) > generation);
        CHECK(draw_widget_pixel(GTK_WIDGET(widget), (int)metrics.cell_width / scale, 0,
                                &pixel));
        CHECK((pixel & UINT32_C(0x00ffffff)) ==
              (index == 0 ? UINT32_C(0x00506070) : UINT32_C(0x00706050)));
        if (index == 0) {
            CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(mouse_output) - 1);
            CHECK(memcmp(output, mouse_output, sizeof(mouse_output) - 1) == 0);
            CHECK(boxrig_terminal_output_consume(terminal, sizeof(mouse_output) - 1) ==
                  BOXRIG_STATUS_OK);
        }
    }

    /* Hold a ready request, then fail backing allocation and immutable preview
     * creation separately. Native geometry has changed even on failure; drawing
     * the old image must not release the old request for that new geometry. */
    observer.width = (int)metrics.cell_width * 8 / scale;
    observer.height = (int)metrics.cell_height * 4 / scale;
    observer.allocations = 0;
    observer.draws = 0;
    observer.inspect_before_draw = false;
    observer.expired = false;
    guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
    gtk_window_resize(window, observer.width, observer.height);
    while (!observer.expired && (observer.allocations == 0 || observer.draws == 0)) {
        (void)g_main_context_iteration(NULL, true);
    }
    if (!observer.expired) {
        g_source_remove(guard);
    }
    CHECK(!observer.expired);
    for (index = 0; index < 2; index++) {
        observer.width = (int)metrics.cell_width * (index == 0 ? 16 : 12) / scale;
        observer.height = (int)metrics.cell_height * (index == 0 ? 8 : 6) / scale;
        observer.allocations = 0;
        observer.draws = 0;
        observer.expired = false;
        failing_surface_width = observer.width * scale;
        failing_surface_height = observer.height * scale;
        failing_surface_after = (unsigned int)index + 1;
        matching_surface_calls = 0;
        generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
        guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
        gtk_window_resize(window, observer.width, observer.height);
        while (!observer.expired && (observer.allocations == 0 || observer.draws == 0)) {
            (void)g_main_context_iteration(NULL, true);
        }
        if (!observer.expired) {
            g_source_remove(guard);
        }
        CHECK(!observer.expired && matching_surface_calls >= failing_surface_after);
        CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) == generation);
        CHECK(!take_grid_resize_action(widget, &resize));
        failing_surface_after = 0;
        observer.draws = 0;
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
        CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) > generation);
        CHECK(!take_grid_resize_action(widget, &resize));
        guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
        while (!observer.expired && observer.draws == 0) {
            (void)g_main_context_iteration(NULL, true);
        }
        if (!observer.expired) {
            g_source_remove(guard);
        }
        CHECK(!observer.expired);
        CHECK(take_grid_resize_action(widget, &resize));
        CHECK(resize.rows == (index == 0 ? 8U : 6U));
        CHECK(resize.columns == (index == 0 ? 16U : 12U));
        CHECK(boxrig_terminal_resize(terminal, resize.rows, resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                  widget, resize.revision, true) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(!frame_result.frame_pending);
    }

    /* One dispatched request can coexist with only the latest undispatched
     * allocation. Pixel-only changes and A -> B -> A must not reuse readiness. */
    for (index = 0; index < sizeof(later_rows) / sizeof(later_rows[0]); index++) {
        observer.width = (int)metrics.cell_width * (int)later_columns[index] / scale +
                         (index == 2 ? 1 : 0);
        observer.height = (int)metrics.cell_height * (int)later_rows[index] / scale;
        observer.allocations = 0;
        observer.draws = 0;
        observer.inspect_before_draw = true;
        observer.early_resize = false;
        observer.expired = false;
        guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
        gtk_window_resize(window, observer.width, observer.height);
        while (!observer.expired && (observer.allocations == 0 || observer.draws == 0)) {
            (void)g_main_context_iteration(NULL, true);
        }
        if (!observer.expired) {
            g_source_remove(guard);
        }
        CHECK(!observer.expired && !observer.early_resize);
        CHECK(observer.offscreen_draw_succeeded);
        CHECK(observer.waiting_frame_status == BOXRIG_UI_OK);
        CHECK(!observer.frame_replaced_preview);
        CHECK(observer.waiting_frame.frame_pending);
        CHECK(!observer.waiting_frame.additional_dispatch);
        if (index == 0) {
            CHECK(take_grid_resize_action(widget, &in_flight));
            CHECK(in_flight.rows == later_rows[index] && in_flight.columns == later_columns[index]);
        } else if (index == 1) {
            CHECK(!take_grid_resize_action(widget, &resize));
            CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, false) == BOXRIG_UI_OK);
            CHECK(boxrig_terminal_resize(terminal, in_flight.rows, in_flight.columns) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                      widget, in_flight.revision, true) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
            CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
            CHECK(frame_result.frame_pending);
            CHECK(!frame_result.additional_dispatch);
            CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) == generation);
            CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
        } else if (index == 2 || index == 5) {
            CHECK(take_grid_resize_action(widget, &resize));
            CHECK(resize.rows == later_rows[index] && resize.columns == later_columns[index]);
            CHECK(resize.revision == (index == 2 ? in_flight.revision + 1 : previous_revision + 3));
            CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                      widget, in_flight.revision, true) == BOXRIG_UI_STALE);
            CHECK(!take_grid_resize_action(widget, &in_flight));
            CHECK(boxrig_terminal_resize(terminal, resize.rows, resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                      widget, resize.revision, true) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
            CHECK(!frame_result.frame_pending);
            previous_revision = resize.revision;
        }
    }

    /* Equal pixel metrics still have a new metrics revision. A pending grid
     * cannot borrow readiness from the previous font allocation. */
    {
        boxrig_font_specification specification;
        boxrig_font_effective_metrics updated_metrics;

        observer.allocations = 0;
        observer.draws = 0;
        observer.early_resize = false;
        observer.expired = false;
        CHECK(boxrig_gtk3_terminal_widget_get_font_specification(widget, &specification) ==
              BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_set_font_specification(widget, &specification) ==
              BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &updated_metrics) ==
              BOXRIG_FONT_CATALOG_OK);
        CHECK(updated_metrics.cell_width == metrics.cell_width &&
              updated_metrics.cell_height == metrics.cell_height &&
              updated_metrics.layout_revision != metrics.layout_revision);
        CHECK(!take_grid_resize_action(widget, &resize));
        guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
        while (!observer.expired && (observer.allocations == 0 || observer.draws == 0)) {
            (void)g_main_context_iteration(NULL, true);
        }
        if (!observer.expired) {
            g_source_remove(guard);
        }
        CHECK(!observer.expired && !observer.early_resize);
        CHECK(observer.offscreen_draw_succeeded && !observer.frame_replaced_preview);
    }

    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, false) == BOXRIG_UI_OK);
    CHECK(!take_grid_resize_action(widget, &resize));
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(frame_result.frame_pending && !frame_result.additional_dispatch);
    observer.draws = 0;
    observer.inspect_before_draw = false;
    observer.expired = false;
    guard = g_timeout_add_seconds(30, resize_boundary_expired, &observer);
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    CHECK(!take_grid_resize_action(widget, &resize));
    while (!observer.expired && observer.draws == 0) {
        (void)g_main_context_iteration(NULL, true);
    }
    if (!observer.expired) {
        g_source_remove(guard);
    }
    CHECK(!observer.expired);
    CHECK(take_grid_resize_action(widget, &resize));
    CHECK(resize.rows == 4 && resize.columns == 8);
    generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
    CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
              widget, resize.revision, false) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
    CHECK(!diagnostic.running && !diagnostic.failed);
    CHECK(!take_grid_resize_action(widget, &resize));
    CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
              widget, resize.revision, true) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == 6 && columns == 12);
    wakeups = observer.wakeups;
    CHECK(feed_bytes(terminal, first_color, sizeof(first_color) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(draw_widget(GTK_WIDGET(widget)));
    CHECK(observer.wakeups == wakeups);
    CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) == generation);

    gtk_widget_destroy(GTK_WIDGET(window));
    CHECK(observer.wakeups == wakeups);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

typedef struct output_scale_observer {
    int scale;
    bool allocated;
    bool drawn;
    bool early_resize;
    bool expired;
} output_scale_observer;

static void output_scale_allocated(
    GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
    output_scale_observer *observer = user_data;
    boxrig_ui_grid_resize resize;

    (void)allocation;
    if (!observer->allocated && gtk_widget_get_scale_factor(widget) == observer->scale) {
        observer->early_resize = take_grid_resize_action(
            BOXRIG_GTK3_TERMINAL_WIDGET(widget), &resize);
        observer->allocated = true;
    }
}

static gboolean output_scale_drawn(GtkWidget *widget, cairo_t *cairo, gpointer user_data)
{
    output_scale_observer *observer = user_data;
    GdkDrawingContext *context = gdk_cairo_get_drawing_context(cairo);

    if (observer->allocated && gtk_widget_get_scale_factor(widget) == observer->scale &&
        context != NULL && gdk_drawing_context_is_valid(context)) {
        observer->drawn = true;
    }
    return FALSE;
}

static gboolean output_scale_expired(gpointer user_data)
{
    output_scale_observer *observer = user_data;

    observer->expired = true;
    return G_SOURCE_REMOVE;
}

static bool test_grid_resize_tracks_runtime_output_scale(void)
{
    static const char setting_name[] = "Gdk/WindowScalingFactor";
    const char *backend = getenv("GDK_BACKEND");
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window;
    boxrig_font_effective_metrics metrics;
    boxrig_font_effective_metrics updated_metrics;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_grid_resize resize;
    output_scale_observer observer = {0};
    Display *display;
    Window owner;
    Atom selection;
    Atom property;
    char selection_name[40];
    unsigned char settings[48] = {0};
    uint16_t name_length = sizeof(setting_name) - 1;
    uint32_t one = 1;
    unsigned int index;
    guint guard;
    bool opengl = getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL;
    if (opengl) widget_config.render_policy = BOXRIG_GTK_RENDER_OPENGL_REQUIRED;

    /* Only the isolated, unscaled X11 gate owns an XSETTINGS manager. The
     * explicit GDK_SCALE and Wayland gates exercise their own native paths. */
    if (backend == NULL || strcmp(backend, "x11") != 0 || getenv("GDK_SCALE") != NULL) {
        return true;
    }
    display = XOpenDisplay(NULL);
    CHECK(display != NULL);
    CHECK(snprintf(selection_name, sizeof(selection_name), "_XSETTINGS_S%d",
                   DefaultScreen(display)) > 0);
    selection = XInternAtom(display, selection_name, False);
    property = XInternAtom(display, "_XSETTINGS_SETTINGS", False);
    CHECK(XGetSelectionOwner(display, selection) == None);
    owner = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    CHECK(owner != None);
    XSelectInput(display, owner, PropertyChangeMask);
    settings[0] = *(const unsigned char *)&one == 1 ? LSBFirst : MSBFirst;
    memcpy(settings + 8, &one, sizeof(one));
    memcpy(settings + 14, &name_length, sizeof(name_length));
    memcpy(settings + 16, setting_name, name_length);

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(model, &widget_config, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(gtk_widget_get_scale_factor(GTK_WIDGET(widget)) == 1);
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) ==
          BOXRIG_FONT_CATALOG_OK);
    /* Leave ample fractional-cell slack so scale-dependent font rounding does
     * not change the one-cell grid under test. */
    gtk_window_set_default_size(window, (int)metrics.cell_width * 3 / 2,
                                (int)metrics.cell_height * 3 / 2);
    observer.scale = 1;
    g_signal_connect_after(widget, "size-allocate", G_CALLBACK(output_scale_allocated), &observer);
    g_signal_connect_after(widget, "draw", G_CALLBACK(output_scale_drawn), &observer);
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    CHECK(wait_for_layout(widget, &layout));
    CHECK(layout.rows == 1 && layout.columns == 1);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    guard = g_timeout_add_seconds(30, output_scale_expired, &observer);
    while (!observer.expired && !observer.drawn) {
        (void)g_main_context_iteration(NULL, true);
    }
    if (!observer.expired) {
        g_source_remove(guard);
    }
    CHECK(!observer.expired && !observer.early_resize);

    for (index = 0; index < 2; index++) {
        uint32_t serial = index + 1;
        int32_t scale = index == 0 ? 2 : 1;

        observer.scale = scale;
        observer.allocated = false;
        observer.drawn = false;
        observer.early_resize = false;
        memcpy(settings + 4, &serial, sizeof(serial));
        memcpy(settings + 40, &serial, sizeof(serial));
        memcpy(settings + 44, &scale, sizeof(scale));
        XChangeProperty(display, owner, property, property, 8, PropModeReplace,
                        settings, sizeof(settings));
        if (index == 0) {
            XEvent timestamp;
            XEvent announcement = {0};

            /* Use the server's property-event timestamp, as required by the
             * XSETTINGS manager protocol, rather than inventing a timestamp. */
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
            CHECK(XSendEvent(display, DefaultRootWindow(display), False,
                             StructureNotifyMask, &announcement) != 0);
        }
        XFlush(display);
        guard = g_timeout_add_seconds(30, output_scale_expired, &observer);
        while (!observer.expired && !observer.drawn) {
            (void)g_main_context_iteration(NULL, true);
        }
        if (!observer.expired) {
            g_source_remove(guard);
        }
        CHECK(!observer.expired && !observer.early_resize);
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &updated_metrics) ==
              BOXRIG_FONT_CATALOG_OK);
        CHECK(updated_metrics.layout_revision != metrics.layout_revision);
        CHECK(wait_for_layout(widget, &layout));
        CHECK(layout.rows == 1 && layout.columns == 1);
        CHECK(take_grid_resize_action(widget, &resize));
        CHECK(resize.rows == 1 && resize.columns == 1);
        if (index == 0) {
            CHECK(resize.revision == 1);
        } else {
            /* Applying smaller metrics to the previous pixel extent can
             * briefly change the low-level layout before the native resize.
             * Only the final one-cell allocation may be applied to the core. */
            CHECK(resize.revision > 1);
        }
        CHECK(boxrig_terminal_resize(terminal, resize.rows, resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(
                  widget, resize.revision, true) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        if (opengl) {
            CHECK(draw_widget(GTK_WIDGET(widget)));
            CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
            boxrig_gtk_render_state state;
            CHECK(boxrig_gtk3_terminal_widget_get_render_state(widget, &state) == BOXRIG_UI_OK);
            CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL);
        }
        CHECK(!frame_result.frame_pending);
        metrics = updated_metrics;
    }
    gtk_widget_destroy(GTK_WIDGET(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    printf("PASS GTK 3 pending grid follows native output scale 1 -> 2 -> 1\n");
    return true;
}

static void adjustment_signal_counted(GtkAdjustment *adjustment, gpointer user_data)
{
    unsigned int *count = user_data;

    (void)adjustment;
    (*count)++;
}

static bool test_layout_frame_and_geometry_hints(void)
{
    static const uint8_t first_text[] = "boxrig";
    static const uint8_t second_text[] = "!";
    static const uint8_t paste_text[] = "abcdef";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window = NULL;
    AtkObject *accessible;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame_result;
    boxrig_ui_dirty dirty;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_layout_action layout_action;
    GtkAdjustment *horizontal;
    GtkAdjustment *vertical;
    int minimum_width;
    int natural_width;
    int minimum_height;
    int natural_height;
    int scale;
    uint64_t generation;

    terminal_config.rows = 3;
    terminal_config.columns = 8;
    terminal_config.output_capacity = 2;
    widget_config.paste_chunk_size = 2;
    CHECK(widget_config.maximum_native_input_events == 256);
    CHECK(widget_config.maximum_frame_items == 256);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(GTK_IS_WIDGET(widget));
    CHECK(GTK_IS_SCROLLABLE(widget));
    CHECK(strcmp(G_OBJECT_TYPE_NAME(widget), "BoxrigGtk3TerminalWidget") == 0);
    accessible = gtk_widget_get_accessible(GTK_WIDGET(widget));
    CHECK(accessible != NULL);
    CHECK(atk_object_get_role(accessible) == ATK_ROLE_TERMINAL);
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    CHECK(scale > 0);
    gtk_widget_show(GTK_WIDGET(widget));
    gtk_widget_get_preferred_width(
        GTK_WIDGET(widget),
        &minimum_width,
        &natural_width);
    gtk_widget_get_preferred_height(
        GTK_WIDGET(widget),
        &minimum_height,
        &natural_height);
    CHECK(minimum_width ==
          (int)((metrics.cell_width + (unsigned int)scale - 1) /
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

    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_resizable(window, false);
    gtk_window_set_default_size(window, natural_width + 3, natural_height + 5);
    CHECK(boxrig_gtk3_terminal_widget_set_window_state(
              widget,
              BOXRIG_UI_WINDOW_NORMAL) == BOXRIG_UI_OK);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    CHECK(wait_for_layout(widget, &layout));
    gtk_widget_grab_focus(GTK_WIDGET(widget));
    while (g_main_context_pending(NULL)) {
        (void)g_main_context_iteration(NULL, false);
    }
    CHECK(accessible_has_state(accessible, ATK_STATE_FOCUSABLE));
    CHECK(accessible_has_state(accessible, ATK_STATE_FOCUSED) ==
          gtk_widget_has_focus(GTK_WIDGET(widget)));
    CHECK(accessible_has_state(accessible, ATK_STATE_ENABLED));
    CHECK(accessible_has_state(accessible, ATK_STATE_SENSITIVE));
    CHECK(!accessible_has_state(accessible, ATK_STATE_BUSY));
    CHECK(ATK_IS_TEXT(accessible));
    CHECK(layout.rows == terminal_config.rows);
    CHECK(layout.columns == terminal_config.columns);
    CHECK(layout.excess_right >= 0 && layout.excess_bottom >= 0);
    CHECK(boxrig_gtk3_terminal_widget_get_size_command_state(
              widget,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_IDLE);

    CHECK(boxrig_gtk3_terminal_widget_set_geometry_hints(
              widget,
              window,
              true) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_set_geometry_hints(
              widget,
              window,
              false) == BOXRIG_UI_OK);

    CHECK(feed_bytes(terminal, first_text, sizeof(first_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_has_presentation(widget));
    generation =
        boxrig_gtk3_terminal_widget_presentation_generation(widget);
    CHECK(generation != 0);
    CHECK(feed_bytes(terminal, second_text, sizeof(second_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_gtk3_terminal_widget_view(widget),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(draw_widget(GTK_WIDGET(widget)));
    CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) ==
          generation);
    CHECK(boxrig_ui_view_peek_dirty(
        boxrig_gtk3_terminal_widget_view(widget),
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_presentation_generation(widget) >
          generation);
    CHECK(!boxrig_ui_view_peek_dirty(
        boxrig_gtk3_terminal_widget_view(widget),
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

    {
        static const uint8_t history_text[] = "\r\none\r\ntwo\r\nthree\r\nfour";
        GtkAdjustment *adjustments[] = {horizontal, vertical};
        unsigned int range_changes[] = {0, 0};
        unsigned int value_changes[] = {0, 0};
        gulong range_handlers[2];
        gulong value_handlers[2];
        GtkAllocation allocation;
        boxrig_scrollbar_model scrollbar;
        bool scrolled = false;
        size_t index;

        for (index = 0; index < 2; index++) {
            range_handlers[index] = g_signal_connect(
                adjustments[index], "changed",
                G_CALLBACK(adjustment_signal_counted), &range_changes[index]);
            value_handlers[index] = g_signal_connect(
                adjustments[index], "value-changed",
                G_CALLBACK(adjustment_signal_counted), &value_changes[index]);
        }
        gtk_widget_get_allocation(GTK_WIDGET(widget), &allocation);
        /* A parent may allocate the same extent again. Stable ranges must not
         * invalidate its scrollbar and start another layout cycle. */
        for (index = 0; index < 3; index++) {
            gtk_widget_queue_resize(GTK_WIDGET(widget));
            gtk_widget_size_allocate(GTK_WIDGET(widget), &allocation);
            CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
            CHECK(range_changes[0] == 0 && range_changes[1] == 0);
            CHECK(value_changes[0] == 0 && value_changes[1] == 0);
        }

        CHECK(feed_bytes(terminal, history_text, sizeof(history_text) - 1));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_model(widget, &scrollbar) == BOXRIG_UI_OK);
        CHECK(scrollbar.history_rows > 0 && scrollbar.position == scrollbar.history_rows);
        CHECK(gtk_adjustment_get_upper(vertical) ==
              (double)(scrollbar.history_rows + scrollbar.viewport_rows));
        CHECK(gtk_adjustment_get_page_size(vertical) == (double)scrollbar.viewport_rows);
        CHECK(gtk_adjustment_get_value(vertical) == (double)scrollbar.position);
        CHECK(range_changes[0] == 0 && range_changes[1] == 1);
        CHECK(value_changes[0] == 0 && value_changes[1] == 1);

        /* Native scrollbar input still updates the terminal viewport. */
        gtk_adjustment_set_value(vertical, 0.0);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_model(widget, &scrollbar) == BOXRIG_UI_OK);
        CHECK(scrollbar.position == 0);
        CHECK(range_changes[1] == 1 && value_changes[1] == 2);
        CHECK(boxrig_gtk3_terminal_widget_apply_scrollbar_action(
                  widget, BOXRIG_SCROLLBAR_POSITION, scrollbar.history_rows, &scrolled) == BOXRIG_UI_OK);
        CHECK(scrolled);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(gtk_adjustment_get_value(vertical) == (double)scrollbar.history_rows);
        CHECK(range_changes[1] == 2 && value_changes[1] == 3);

        /* Compare the actual adjustment, not a cached last published value:
         * callers can modify the GtkScrollable properties between frames. */
        g_object_set(horizontal, "step-increment", 2.0, NULL);
        range_changes[0] = 0;
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(gtk_adjustment_get_step_increment(horizontal) == 1.0);
        CHECK(range_changes[0] == 1 && value_changes[0] == 0);
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
        CHECK(range_changes[0] == 1 && range_changes[1] == 2);
        CHECK(value_changes[0] == 0 && value_changes[1] == 3);
        for (index = 0; index < 2; index++) {
            g_signal_handler_disconnect(adjustments[index], range_handlers[index]);
            g_signal_handler_disconnect(adjustments[index], value_handlers[index]);
        }
    }

    CHECK(boxrig_gtk3_terminal_widget_set_preferred_grid(widget, 4, 10) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_size_command_state(
              widget,
              &command_state) == BOXRIG_UI_OK);
    CHECK(command_state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(boxrig_gtk3_terminal_widget_take_layout_action(
        widget, &layout_action));
    CHECK(layout_action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    CHECK(layout_action.value.content_size.cause ==
          BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID);
    CHECK(layout_action.value.content_size.width > 0 &&
          layout_action.value.content_size.height > 0);
    CHECK(!boxrig_gtk3_terminal_widget_take_layout_action(
        widget, &layout_action));

    CHECK(boxrig_gtk3_terminal_widget_queue_paste(
              widget,
              paste_text,
              sizeof(paste_text) - 1) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(accessible_has_state(accessible, ATK_STATE_BUSY));
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(frame_result.input_blocked);
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    CHECK(!accessible_has_state(accessible, ATK_STATE_BUSY));
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);

    CHECK(boxrig_ui_view_set_focused(
              boxrig_gtk3_terminal_widget_view(widget), true) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_quiesce(widget) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_focused(boxrig_gtk3_terminal_widget_view(widget)));
    CHECK(!accessible_has_state(accessible, ATK_STATE_BUSY));
    CHECK(!accessible_has_state(accessible, ATK_STATE_ENABLED));
    CHECK(!accessible_has_state(accessible, ATK_STATE_SENSITIVE));
    gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_destroy(GTK_WIDGET(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_clipboard_actions(void)
{
    static const uint8_t copied_text[] = "copy";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window = NULL;
    GdkDisplay *display;
    GtkClipboard *primary;
    GtkClipboard *clipboard;
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
    size_t accepted_pastes = 2;
    boxrig_ui_status third_paste_status;

    display = gdk_display_get_default();
    CHECK(display != NULL);
    if (gdk_display_get_default_seat(display) == NULL) {
        return true;
    }
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    widget_config.paste_capacity = 4;
    widget_config.paste_chunk_size = 2;
    widget_config.maximum_clipboard_transfers = 2;
    widget_config.maximum_clipboard_bytes = 10;
    if (getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL)
        widget_config.render_policy = BOXRIG_GTK_RENDER_OPENGL_REQUIRED;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           boxrig_gtk3_terminal_widget_get_layout(widget, &layout) ==
               BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(layout.rows == terminal_config.rows &&
          layout.columns == terminal_config.columns);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);

    CHECK(gtk_widget_get_display(GTK_WIDGET(widget)) == display);
    capabilities =
        boxrig_gtk3_terminal_widget_view_action_capabilities(widget);

    CHECK(feed_bytes(terminal, copied_text, sizeof(copied_text) - 1));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(
              widget,
              &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_gtk3_terminal_widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_hit_test(
              widget,
              (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE / 2,
              (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE / 2,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.text_position_valid);
    CHECK(boxrig_ui_view_begin_selection(
              boxrig_gtk3_terminal_widget_view(widget),
              BOXRIG_UI_SELECTION_LINE,
              &hit) == BOXRIG_UI_OK);

    primary = gtk_clipboard_get_for_display(display, GDK_SELECTION_PRIMARY);
    clipboard = gtk_clipboard_get_for_display(
        display,
        GDK_SELECTION_CLIPBOARD);
    CHECK(primary != NULL && clipboard != NULL && primary != clipboard);
    CHECK(capabilities.copy_targets ==
          (BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
           BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD));
    capabilities = boxrig_gtk3_terminal_widget_view_action_capabilities(widget);
    CHECK(capabilities.paste_targets == 0 && !capabilities.reconversion);

    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == 10);

    memset(&read_result, 0, sizeof(read_result));
    gtk_clipboard_request_text(primary, clipboard_text_read, &read_result);
    while (!read_result.completed) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(read_result.text != NULL && strcmp(read_result.text, "copy\n") == 0);
    g_free(read_result.text);
    memset(&read_result, 0, sizeof(read_result));
    gtk_clipboard_request_text(clipboard, clipboard_text_read, &read_result);
    while (!read_result.completed) {
        (void)g_main_context_iteration(NULL, true);
    }
    CHECK(read_result.text != NULL && strcmp(read_result.text, "copy\n") == 0);
    g_free(read_result.text);

    /* Failure must preserve the complete previous native offer, for either target. */
    boxrig_ui_view *copy_view = boxrig_gtk3_terminal_widget_view(widget);
    const boxrig_ui_selection_options rectangle = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
    };
    CHECK(boxrig_gtk3_terminal_widget_hit_test(widget, BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(copy_view, &rectangle, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_hit_test(widget,
        ((boxrig_ui_fixed)metrics.cell_width * 8 - 1) * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(copy_view, &hit) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result copy_focus = hit;
    GtkClipboard *targets[] = {primary, clipboard};
    const boxrig_ui_clipboard_target target_kinds[] = {
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION, BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD
    };
    for (size_t target = 0; target < 2; target++) {
        action.clipboard_target = target_kinds[target];
        for (size_t failure = 0; failure < 2; failure++) {
            CHECK(boxrig_ui_view_set_rectangular_copy_policy(copy_view, failure == 0
                ? BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES
                : BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES) == BOXRIG_UI_OK);
            if (failure == 1) boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 1);
            CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
                (failure == 0 ? BOXRIG_UI_LIMIT_EXCEEDED : BOXRIG_UI_OUT_OF_MEMORY));
            memset(&read_result, 0, sizeof(read_result));
            gtk_clipboard_request_text(targets[target], clipboard_text_read, &read_result);
            while (!read_result.completed) (void)g_main_context_iteration(NULL, true);
            CHECK(read_result.text != NULL && strcmp(read_result.text, "copy\n") == 0);
            g_free(read_result.text);
        }
        CHECK(boxrig_gtk3_terminal_widget_hit_test(widget, BOXRIG_UI_FIXED_ONE,
            BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_selection(copy_view, &hit) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) == BOXRIG_UI_UNAVAILABLE);
        memset(&read_result, 0, sizeof(read_result));
        gtk_clipboard_request_text(targets[target], clipboard_text_read, &read_result);
        while (!read_result.completed) (void)g_main_context_iteration(NULL, true);
        CHECK(read_result.text != NULL && strcmp(read_result.text, "copy\n") == 0);
        g_free(read_result.text);
        CHECK(boxrig_ui_view_update_selection(copy_view, &copy_focus) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_ui_view_begin_selection(copy_view, BOXRIG_UI_SELECTION_LINE, &hit) == BOXRIG_UI_OK);

    CHECK(boxrig_gtk3_terminal_widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
    gtk_clipboard_set_text(primary, "P\r\n", -1);
    gtk_clipboard_set_text(clipboard, "P\r\n", -1);
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.outgoing_clipboard_bytes == 0);
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.active_clipboard_transfers <= 2);
    third_paste_status =
        boxrig_gtk3_terminal_widget_apply_view_action(widget, &action);
    if (diagnostic.active_clipboard_transfers == 2) {
        CHECK(third_paste_status == BOXRIG_UI_BUSY);
    } else {
        CHECK(third_paste_status == BOXRIG_UI_OK);
        accepted_pastes++;
    }
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.active_clipboard_transfers <= 2);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(
                  widget,
                  &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(diagnostic.native_input_queue_depth == accepted_pastes);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) ==
          BOXRIG_UI_OK);
    CHECK(!frame_result.input_blocked);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == accepted_pastes * 2);
    CHECK(memcmp(output, "P\nP\nP\n", output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    gtk_clipboard_set_text(clipboard, "drop", -1);
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    gtk_widget_hide(GTK_WIDGET(window));
    CHECK(!gtk_widget_get_mapped(GTK_WIDGET(widget)));
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(
                  widget,
                  &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_UNAVAILABLE);
    gtk_widget_show_all(GTK_WIDGET(window));
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget))) {
        (void)g_main_context_iteration(NULL, true);
    }

    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    gtk_clipboard_set_text(primary, "12345", -1);
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    while (diagnostic.active_clipboard_transfers != 0) {
        (void)g_main_context_iteration(NULL, true);
        CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(
                  widget,
                  &diagnostic) == BOXRIG_UI_OK);
    }
    CHECK(diagnostic.failed);
    CHECK(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_CLIPBOARD);

    gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_destroy(GTK_WIDGET(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_delayed_clipboard_callback(bool dispose)
{
    static const unsigned char delayed_text[] = "late";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config widget_config =
        boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    GtkWindow *window = NULL;
    GtkClipboard *clipboard;
    Display *selection_display = NULL;
    Window selection_owner = None;
    Atom clipboard_atom;
    Atom utf8_string;
    Atom property;
    XEvent request;
    XEvent notify;
    boxrig_ui_layout_result layout;
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_ui_view_action action;

    if (gdk_display_get_default_seat(gdk_display_get_default()) == NULL) {
        return true;
    }
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(
              model,
              &widget_config,
              &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    CHECK(window != NULL);
    g_object_ref_sink(window);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) ==
          BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
           boxrig_gtk3_terminal_widget_get_layout(widget, &layout) ==
               BOXRIG_UI_INVALID_ARGUMENT) {
        (void)g_main_context_iteration(NULL, true);
    }
    selection_display = XOpenDisplay(NULL);
    CHECK(selection_display != NULL);
    clipboard_atom = XInternAtom(selection_display, "CLIPBOARD", False);
    utf8_string = XInternAtom(selection_display, "UTF8_STRING", False);
    selection_owner = XCreateSimpleWindow(
        selection_display,
        RootWindow(selection_display, DefaultScreen(selection_display)),
        0,
        0,
        1,
        1,
        0,
        0,
        0);
    CHECK(clipboard_atom != None && utf8_string != None &&
          selection_owner != None);
    XSetSelectionOwner(
        selection_display,
        clipboard_atom,
        selection_owner,
        CurrentTime);
    XSync(selection_display, False);
    CHECK(XGetSelectionOwner(selection_display, clipboard_atom) ==
          selection_owner);

    clipboard = gtk_clipboard_get_for_display(
        gtk_widget_get_display(GTK_WIDGET(widget)),
        GDK_SELECTION_CLIPBOARD);
    CHECK(clipboard != NULL);
    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) ==
          BOXRIG_UI_OK);
    gdk_display_flush(gtk_widget_get_display(GTK_WIDGET(widget)));
    XNextEvent(selection_display, &request);
    CHECK(request.type == SelectionRequest);
    CHECK(request.xselectionrequest.selection == clipboard_atom);
    CHECK(request.xselectionrequest.target == utf8_string);
    property = request.xselectionrequest.property == None
                   ? request.xselectionrequest.target
                   : request.xselectionrequest.property;
    CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
          BOXRIG_UI_OK);
    CHECK(diagnostic.active_clipboard_transfers == 1);

    if (dispose) {
        CHECK(boxrig_gtk3_terminal_widget_quiesce(widget) == BOXRIG_UI_OK);
        gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(widget));
        gtk_widget_destroy(GTK_WIDGET(window));
        g_object_unref(window);
        g_object_unref(widget);
    } else {
        CHECK(boxrig_gtk3_terminal_widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_apply_view_action(widget, &action) == BOXRIG_UI_UNAVAILABLE);
        CHECK(boxrig_gtk3_terminal_widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
    }

    XChangeProperty(
        selection_display,
        request.xselectionrequest.requestor,
        property,
        utf8_string,
        8,
        PropModeReplace,
        delayed_text,
        (int)(sizeof(delayed_text) - 1));
    memset(&notify, 0, sizeof(notify));
    notify.xselection.type = SelectionNotify;
    notify.xselection.display = selection_display;
    notify.xselection.requestor = request.xselectionrequest.requestor;
    notify.xselection.selection = request.xselectionrequest.selection;
    notify.xselection.target = request.xselectionrequest.target;
    notify.xselection.property = property;
    notify.xselection.time = request.xselectionrequest.time;
    CHECK(XSendEvent(
              selection_display,
              notify.xselection.requestor,
              False,
              0,
              &notify) != 0);
    XSync(selection_display, False);
    gdk_display_sync(gdk_display_get_default());
    (void)g_main_context_iteration(NULL, true);
    while (g_main_context_pending(NULL)) {
        (void)g_main_context_iteration(NULL, false);
    }
    if (!dispose) {
        do {
            CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
            if (diagnostic.active_clipboard_transfers == 0) break;
            (void)g_main_context_iteration(NULL, true);
        } while (true);
        boxrig_ui_component_frame_result frame;
        const uint8_t *output;
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        CHECK(boxrig_gtk3_terminal_widget_quiesce(widget) == BOXRIG_UI_OK);
        gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(widget));
        gtk_widget_destroy(GTK_WIDGET(window));
        g_object_unref(window);
        g_object_unref(widget);
    }
    XDestroyWindow(selection_display, selection_owner);
    XCloseDisplay(selection_display);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}


static bool settle_scrollbar_visibility(BoxrigGtk3TerminalWidget *widget, boxrig_terminal *terminal,
    unsigned int *resizes_out)
{
    *resizes_out = 0;
    boxrig_ui_layout_result initial;
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
        boxrig_gtk3_terminal_widget_get_layout(widget, &initial) != BOXRIG_UI_OK)
        (void)g_main_context_iteration(NULL, true);
    for (;;) {
        while (g_main_context_pending(NULL)) (void)g_main_context_iteration(NULL, false);
        boxrig_ui_component_frame_result frame;
        CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
        boxrig_ui_layout_action action;
        if (boxrig_gtk3_terminal_widget_take_layout_action(widget, &action)) {
            CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
            CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
                action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
            CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(widget, action.revision, true) == BOXRIG_UI_OK);
            CHECK(++*resizes_out < 4);
            continue;
        }
        boxrig_ui_scrollbar_visibility_state state;
        CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
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

static void scrollbar_scale_allocated(GtkWidget *widget, GtkAllocation *allocation, gpointer data)
{
    (void)allocation;
    int *expected = data;
    if (gtk_widget_get_scale_factor(widget) == *expected) *expected = 0;
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
    BoxrigGtk3TerminalWidget *widget = NULL;
    boxrig_gtk3_terminal_widget_config wc = boxrig_gtk3_terminal_widget_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkScrolledWindow *container = boxrig_gtk3_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    GtkWindow *window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 420, 200);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(container));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    unsigned int resizes;
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    for (unsigned int index = 0; index < 2; index++) {
        boxrig_font_effective_metrics before, after;
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &before) == BOXRIG_FONT_CATALOG_OK);
        boxrig_ui_scrollbar_visibility policy = index == 0 ? BOXRIG_UI_SCROLLBAR_NEVER : BOXRIG_UI_SCROLLBAR_ALWAYS;
        CHECK(boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, policy) == BOXRIG_UI_OK);
        boxrig_ui_scrollbar_visibility_state bar;
        CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &bar) == BOXRIG_UI_OK);
        CHECK(bar.pending);
        boxrig_font_specification font = boxrig_font_specification_default();
        CHECK(boxrig_gtk3_terminal_widget_set_font_specification(widget, &font) == BOXRIG_UI_BUSY);
        bool changed = true;
        CHECK(boxrig_gtk3_terminal_widget_visual_zoom(widget, BOXRIG_FONT_ZOOM_INCREASE,
            boxrig_font_visual_step_policy_default(), &changed) == BOXRIG_UI_BUSY);
        CHECK(!changed);
        CHECK(boxrig_gtk3_terminal_widget_visual_zoom_reset(widget, &changed) == BOXRIG_UI_BUSY);
        uint32_t serial = index + 1;
        int32_t scale = index == 0 ? 2 : 1;
        int expected_scale = scale;
        gulong allocated_signal = g_signal_connect_after(widget, "size-allocate", G_CALLBACK(scrollbar_scale_allocated), &expected_scale);
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
        while (!expired && expected_scale != 0)
            (void)g_main_context_iteration(NULL, true);
        if (!expired) g_source_remove(guard);
        g_signal_handler_disconnect(widget, allocated_signal);
        CHECK(!expired);
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &after) == BOXRIG_FONT_CATALOG_OK);
        CHECK(after.layout_revision == before.layout_revision);
        CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
        CHECK(resizes > 0 && resizes <= 2);
        CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &bar) == BOXRIG_UI_OK);
        CHECK(!bar.pending && bar.visible == (policy == BOXRIG_UI_SCROLLBAR_ALWAYS));
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &after) == BOXRIG_FONT_CATALOG_OK);
        CHECK(after.layout_revision != before.layout_revision);
        size_t rows, columns;
        boxrig_terminal_size(terminal, &rows, &columns);
        CHECK(columns == (size_t)gtk_widget_get_allocated_width(GTK_WIDGET(widget)) * scale / after.cell_width);
        CHECK(rows == (size_t)gtk_widget_get_allocated_height(GTK_WIDGET(widget)) * scale / after.cell_height);
        CHECK(bar.reserved_width == (unsigned int)(gtk_widget_get_allocated_width(GTK_WIDGET(container)) -
            gtk_widget_get_allocated_width(GTK_WIDGET(widget))));
    }
    gtk_widget_destroy(GTK_WIDGET(window));
    /* The parent resets GtkScrollable adjustments while removing a disposed
     * child. That notification must not recreate owned objects. */
    CHECK(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(widget)) == NULL);
    CHECK(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(widget)) == NULL);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    while (g_main_context_pending(NULL)) (void)g_main_context_iteration(NULL, false);
    puts("PASS GTK 3 scrollbar transaction survives native scale 1 -> 2 -> 1");
    return true;
}

static bool test_standard_scrollbar_visibility(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 8; config.columns = 30;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    boxrig_gtk3_terminal_widget_config wc = boxrig_gtk3_terminal_widget_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkScrolledWindow *container = boxrig_gtk3_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    g_object_ref_sink(container);
    CHECK(boxrig_gtk3_terminal_widget_create_scroll_container(widget) == NULL);
    GtkWindow *window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    g_object_ref_sink(window);
    gtk_window_set_decorated(window, false);
    gtk_window_set_default_size(window, 420, 200);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(container));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(GTK_WIDGET(window));
    unsigned int resizes;
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    boxrig_ui_scrollbar_visibility_state state;
    CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.visible && state.requested == BOXRIG_UI_SCROLLBAR_ALWAYS && state.reserved_width > 0);
    const int original_width = gtk_widget_get_allocated_width(GTK_WIDGET(widget));
    const int outer_width = gtk_widget_get_allocated_width(GTK_WIDGET(container));
    CHECK((unsigned int)(outer_width - original_width) == state.reserved_width);
    CHECK(boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.pending && state.visible);
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 1);
    CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
    CHECK(gtk_widget_get_allocated_width(GTK_WIDGET(widget)) == outer_width);
    size_t rows, columns;
    boxrig_terminal_size(terminal, &rows, &columns);
    boxrig_ui_layout_result layout;
    CHECK(boxrig_gtk3_terminal_widget_get_layout(widget, &layout) == BOXRIG_UI_OK);
    CHECK(rows == layout.rows && columns == layout.columns);
    CHECK(boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_AUTO) == BOXRIG_UI_OK);
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 0);
    for (size_t i = 0; i < 30; i++) CHECK(feed_bytes(terminal, (const uint8_t *)"row\r\n", 5));
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 1);
    CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.visible && state.reserved_width > 0 && !state.pending);
    CHECK(gtk_widget_get_allocated_width(GTK_WIDGET(widget)) == original_width);
    CHECK(boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_ALWAYS) == BOXRIG_UI_OK);
    CHECK(feed_bytes(terminal, (const uint8_t *)"\033[?1049h", 8));
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(resizes == 0);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(container);
    CHECK(gtk_adjustment_get_upper(adjustment) == gtk_adjustment_get_page_size(adjustment));
    CHECK(gtk_adjustment_get_value(adjustment) == 0);
    CHECK(feed_bytes(terminal, (const uint8_t *)"\033[?1049l", 8));
    CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
    CHECK(boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_AUTO) == BOXRIG_UI_OK);
    const char *sequences[] = {"\033[?1049h", "\033[?1049l", "\033[3J"};
    for (size_t i = 0; i < 3; i++) {
        CHECK(feed_bytes(terminal, (const uint8_t *)sequences[i], strlen(sequences[i])));
        CHECK(settle_scrollbar_visibility(widget, terminal, &resizes));
        CHECK(resizes == 1);
        CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
        CHECK(state.visible == (i == 1) && !state.pending);
    }
    gtk_container_remove(GTK_CONTAINER(container), GTK_WIDGET(widget));
    gtk_container_remove(GTK_CONTAINER(window), GTK_WIDGET(container));
    g_object_unref(container);
    /* A caller-owned container remains entirely under the caller's policy. */
    GtkScrolledWindow *external = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    g_object_ref_sink(external);
    gtk_scrolled_window_set_policy(external, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_container_add(GTK_CONTAINER(external), GTK_WIDGET(widget));
    CHECK(boxrig_gtk3_terminal_widget_set_scrollbar_visibility(widget, BOXRIG_UI_SCROLLBAR_NEVER) == BOXRIG_UI_OK);
    GtkPolicyType horizontal, vertical;
    gtk_scrolled_window_get_policy(external, &horizontal, &vertical);
    CHECK(vertical == GTK_POLICY_ALWAYS);
    CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(!state.visible && state.reserved_width == 0 && !state.pending);
    gtk_container_remove(GTK_CONTAINER(external), GTK_WIDGET(widget));
    g_object_unref(external);
    container = boxrig_gtk3_terminal_widget_create_scroll_container(widget);
    CHECK(container != NULL);
    g_object_ref_sink(container);
    CHECK(boxrig_gtk3_terminal_widget_get_scrollbar_visibility(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.requested == BOXRIG_UI_SCROLLBAR_NEVER && !state.visible && !state.pending);
    gtk_container_remove(GTK_CONTAINER(container), GTK_WIDGET(widget));
    g_object_unref(container);
    gtk_widget_destroy(GTK_WIDGET(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    printf("PASS GTK 3 standard scrollbar visibility, native width, single resize and external ownership\n");
    return true;
}

/* Protocol decorations must not invalidate otherwise usable preedit text. */
static bool test_preedit_clause_attributes(void)
{
    static const char text[] = "A記者の🙂汽車がZ";
    boxrig_gtk3_preedit_translation translation;
    PangoAttrList *attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_foreground_new(65535, 65535, 65535), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 14, 23);
    /* Reproduce malformed old GTK Wayland endpoints and a split UTF-8 scalar. */
    add_preedit_attribute(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD), 3, 1);
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 2, 3);
    CHECK(boxrig_gtk3_input_translate_preedit(text, attributes, 5, &translation));
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
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 1, 7);
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 14, 23);
    CHECK(boxrig_gtk3_input_translate_preedit(text, attributes, 1, &translation));
    CHECK(translation.selection_length == 0);
    CHECK(translation.segment_count == 2);
    CHECK(translation.segments[0].text_offset == 1 && translation.segments[0].text_length == 6);
    CHECK(translation.segments[1].text_offset == 14 && translation.segments[1].text_length == 9);
    for (size_t index = 0; index < translation.segment_count; index++)
        CHECK((translation.segments[index].feedback & BOXRIG_COMPOSITION_FEEDBACK_REVERSE) != 0);
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_foreground_new(100, 100, 100), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_underline_new(PANGO_UNDERLINE_NONE), 0, G_MAXUINT);
    add_preedit_attribute(attributes, pango_attr_weight_new(PANGO_WEIGHT_NORMAL), 0, G_MAXUINT);
    CHECK(boxrig_gtk3_input_translate_preedit("abc", attributes, 1, &translation));
    CHECK(translation.caret_offset == 1 && translation.selection_length == 0 && translation.segment_count == 0);
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);
    /* Attribute indices are scalar boundaries, independent of grapheme layout. */
    attributes = pango_attr_list_new();
    add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), 1, 3);
    CHECK(boxrig_gtk3_input_translate_preedit("é語", attributes, 1, &translation));
    CHECK(translation.caret_offset == 1 && translation.selection_offset == 1 && translation.selection_length == 2);
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);

    char many[1025];
    memset(many, 'a', sizeof(many) - 1); many[sizeof(many) - 1] = '\0';
    attributes = pango_attr_list_new();
    for (guint offset = 0; offset + 1 < sizeof(many); offset += 2)
        add_preedit_attribute(attributes, pango_attr_background_new(0, 0, 0), offset, offset + 1);
    CHECK(boxrig_gtk3_input_translate_preedit(many, attributes, 10, &translation));
    CHECK(translation.caret_offset == 10 && translation.selection_length == 0 && translation.segment_count == 0);
    boxrig_gtk3_input_clear_preedit_translation(&translation);
    pango_attr_list_unref(attributes);
    return true;
}

typedef struct printable_router_observation {
    boxrig_key_action actions[4];
    size_t captured_count;
    size_t terminal_count;
} printable_router_observation;

static boxrig_input_key_route route_printable_key(
    const boxrig_key_event *event, void *context)
{
    printable_router_observation *observation = context;
    if (event->shifted_codepoint == '+' && event->modifiers == BOXRIG_MODIFIER_SHIFT) {
        if (observation->captured_count < 4)
            observation->actions[observation->captured_count] = event->action;
        observation->captured_count++;
        /* Captured repeats/releases must stay captured even if this changes. */
        return event->action == BOXRIG_KEY_PRESS
            ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION : BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    observation->terminal_count++;
    return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
}

static bool test_printable_key_router_precedes_native_ime(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_gtk3_terminal_widget_config config = boxrig_gtk3_terminal_widget_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    printable_router_observation observation = {0};
    boxrig_ui_component_frame_result frame_result;
    config.ime_policy = BOXRIG_GTK_IME_GTK;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_create(model, &config, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_show_all(window);
    CHECK(boxrig_gtk3_terminal_widget_set_key_router(widget, route_printable_key, &observation) == BOXRIG_UI_OK);
    GdkEventFocus focus = {0};
    focus.type = GDK_FOCUS_CHANGE;
    focus.window = gtk_widget_get_window(GTK_WIDGET(widget));
    focus.in = TRUE;
    gboolean handled = FALSE;
    g_signal_emit_by_name(widget, "focus-in-event", &focus, &handled);
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    const guint symbols[] = {GDK_KEY_plus, GDK_KEY_a, GDK_KEY_b};
    for (size_t index = 0; index < 3; index++) {
        GdkKeymapKey *keys = NULL;
        gint count = 0;
        CHECK(gdk_keymap_get_entries_for_keyval(
            gdk_keymap_get_for_display(gtk_widget_get_display(GTK_WIDGET(widget))),
            symbols[index], &keys, &count));
        CHECK(count > 0);
        GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
        event->key.window = g_object_ref(focus.window);
        event->key.keyval = symbols[index];
        event->key.hardware_keycode = keys[0].keycode;
        event->key.group = keys[0].group;
        event->key.state = index == 0 ? GDK_SHIFT_MASK : 0;
        g_free(keys);
        g_signal_emit_by_name(widget, "key-press-event", event, &handled);
        CHECK(handled);
        if (index < 2) {
            g_signal_emit_by_name(widget, "key-press-event", event, &handled);
            CHECK(handled);
        }
        event->type = GDK_KEY_RELEASE;
        g_signal_emit_by_name(widget, "key-release-event", event, &handled);
        CHECK(handled);
        gdk_event_free(event);
    }
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &frame_result) == BOXRIG_UI_OK);
    CHECK(observation.captured_count == 3);
    CHECK(observation.actions[0] == BOXRIG_KEY_PRESS);
    CHECK(observation.actions[1] == BOXRIG_KEY_REPEAT);
    CHECK(observation.actions[2] == BOXRIG_KEY_RELEASE);
    CHECK(observation.terminal_count == 2);
    const uint8_t *output = NULL;
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "aab", 3) == 0);
    CHECK(boxrig_gtk3_terminal_widget_set_key_router(widget, NULL, NULL) == BOXRIG_UI_OK);
    gtk_widget_destroy(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    printf("PASS GTK 3 printable routing captures press/repeat/release before native IME\n");
    return true;
}

static bool test_native_text_before_shutdown(void)
{
    /* GTK's default font map also serves labels and window decorations.
     * Draw a label offscreen so cleanup is exercised on X11 as well as Wayland. */
    GtkWidget *label = gtk_label_new("Native text before shutdown");
    g_object_ref_sink(label);
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 320, 40);
    cairo_t *context = cairo_create(surface);
    pango_cairo_show_layout(context, gtk_label_get_layout(GTK_LABEL(label)));
    cairo_status_t status = cairo_status(context);
    cairo_destroy(context);
    cairo_surface_destroy(surface);
    gtk_widget_destroy(label);
    g_object_unref(label);
    CHECK(status == CAIRO_STATUS_SUCCESS);
    return true;
}

int main(int argc, char **argv)
{
    const boxrig_test_frontend_component_operations operations = {
        .frontend_name = "GTK 3",
        .lifecycle = {
            .create_host = gtk3_fixture_create_host,
            .destroy_host = gtk3_fixture_destroy_host,
            .create_component = gtk3_fixture_create_component,
            .quiesce_component = gtk3_fixture_quiesce_component,
            .destroy_component = gtk3_fixture_destroy_component},
        .layout = {
            .component_view = gtk3_fixture_component_view,
            .get_effective_metrics = gtk3_fixture_get_effective_metrics,
            .allocate = gtk3_fixture_allocate,
            .set_preferred_grid = gtk3_fixture_set_preferred_grid,
            .get_preferred_grid = gtk3_fixture_get_preferred_grid,
            .get_size_command_state =
                gtk3_fixture_get_size_command_state,
            .take_layout_action = gtk3_fixture_take_layout_action,
            .complete_size_command =
                gtk3_fixture_complete_size_command,
            .complete_grid_resize =
                gtk3_fixture_complete_grid_resize,
            .set_visible = gtk3_fixture_set_visible},
        .frame = {
            .set_frame_request_callback =
                gtk3_fixture_set_frame_request_callback,
            .frame = gtk3_fixture_frame},
        .input = {
            .queue_paste = gtk3_fixture_queue_paste,
            .run_native_transcript =
                gtk3_fixture_run_native_input_transcript},
        .font = {
            .fixture_font_path = argc == 2 ? argv[1] : NULL,
            .register_font_file = gtk3_fixture_register_font_file,
            .set_specification = gtk3_fixture_set_font_specification,
            .get_specification = gtk3_fixture_get_font_specification,
            .primary_resource = gtk3_fixture_primary_font_resource,
            .catalog_revision = gtk3_fixture_font_catalog_revision,
            .set_line_height = gtk3_fixture_set_line_height,
            .get_line_height = gtk3_fixture_get_line_height,
            .visual_zoom = gtk3_fixture_visual_zoom,
            .visual_zoom_reset = gtk3_fixture_visual_zoom_reset,
            .set_text_direction = gtk3_fixture_set_text_direction,
            .text_direction = gtk3_fixture_text_direction},
        .view_action = {
            .copy_selection = gtk3_fixture_copy_selection,
            .capabilities = gtk3_fixture_view_action_capabilities,
            .apply = gtk3_fixture_apply_view_action,
            .get_scrollbar_model = gtk3_fixture_get_scrollbar_model,
            .get_scrollbar_state = gtk3_fixture_get_scrollbar_state,
            .apply_scrollbar_action =
                gtk3_fixture_apply_scrollbar_action},
        .presentation = {
            .set_transparency = gtk3_fixture_set_transparency,
            .get_transparency = gtk3_fixture_get_transparency,
            .capabilities = gtk3_fixture_capabilities,
            .get_diagnostic = gtk3_fixture_get_diagnostic,
            .composition = gtk3_fixture_composition,
            .observe = gtk3_fixture_observe_presentation}};

    if (argc != 2) {
        fprintf(stderr, "usage: test_gtk3_widget FONT_FILE\n");
        return 2;
    }
    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "FAIL GTK initialization\n");
        return 1;
    }
    if (!test_native_text_before_shutdown()) {
        return 1;
    }
    if (getenv("BOXRIG_TEST_SCROLLBAR_ONLY") != NULL) {
        bool passed = test_standard_scrollbar_visibility() && test_scrollbar_visibility_during_scale_change();
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
    bool passed = test_printable_key_router_precedes_native_ime() && (getenv("BOXRIG_TEST_RENDER_OPENGL") != NULL
        ? boxrig_test_run_frontend_component_fixture(&operations) &&
          test_grid_resize_tracks_runtime_output_scale() && test_clipboard_actions()
        : test_standard_scrollbar_visibility() && test_scrollbar_visibility_during_scale_change() &&
                  boxrig_test_run_frontend_component_fixture(&operations) &&
                  test_preedit_clause_attributes() && test_preedit_translation() &&
                  test_pointer_router_retains_fractional_hit() &&
                  test_resize_preview_covers_expanded_allocation() &&
                  test_grid_resize_tracks_native_allocation_presentation() &&
                  test_grid_resize_tracks_runtime_output_scale() &&
                  test_layout_frame_and_geometry_hints() &&
                  test_clipboard_actions() &&
                  test_delayed_clipboard_callback(false) && test_delayed_clipboard_callback(true));

    while (g_main_context_pending(NULL)) {
        (void)g_main_context_iteration(NULL, false);
    }
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
        printf("PASS GTK 3 frontend component conformance and native contracts\n");
    }
    return passed ? 0 : 1;
}
