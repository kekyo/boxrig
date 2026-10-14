/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"
#include "boxrig/posix.h"
#include <math.h>

#include "boxrig/ui/shortcut.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static bool fixed_to_logical_pixels(
    boxrig_ui_fixed value,
    gint scale,
    gint *pixels_out)
{
    uint64_t divisor;
    uint64_t fixed;
    uint64_t pixels;

    if (value <= 0 || scale <= 0 || pixels_out == NULL) {
        return false;
    }
    divisor = (uint64_t)(unsigned int)scale * BOXRIG_UI_FIXED_ONE;
    fixed = (uint64_t)value;
    pixels = fixed / divisor + (fixed % divisor != 0 ? 1 : 0);
    if (pixels == 0 || pixels > G_MAXINT) {
        return false;
    }
    *pixels_out = (gint)pixels;
    return true;
}

static bool widget_allocation_fixed(
    const boxrig_term_gtk4_application *application,
    boxrig_ui_fixed *width_out,
    boxrig_ui_fixed *height_out)
{
    gint width;
    gint height;
    gint scale;
    uint64_t fixed_scale;

    if (application == NULL || application->widget == NULL ||
        width_out == NULL || height_out == NULL) {
        return false;
    }
    width = gtk_widget_get_width(GTK_WIDGET(application->widget));
    height = gtk_widget_get_height(GTK_WIDGET(application->widget));
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(application->widget));
    if (width <= 0 || height <= 0 || scale <= 0) {
        return false;
    }
    fixed_scale = (uint64_t)(unsigned int)scale * BOXRIG_UI_FIXED_ONE;
    if ((uint64_t)(unsigned int)width >
            (uint64_t)INT64_MAX / fixed_scale ||
        (uint64_t)(unsigned int)height >
            (uint64_t)INT64_MAX / fixed_scale) {
        return false;
    }
    *width_out = (boxrig_ui_fixed)(unsigned int)width *
                 (boxrig_ui_fixed)fixed_scale;
    *height_out = (boxrig_ui_fixed)(unsigned int)height *
                  (boxrig_ui_fixed)fixed_scale;
    return true;
}

static bool native_window_is_normal(
    const boxrig_term_gtk4_application *application)
{
    GdkToplevelState state;

    if (application->surface == NULL ||
        !GDK_IS_TOPLEVEL(application->surface)) {
        return false;
    }
    state = gdk_toplevel_get_state(GDK_TOPLEVEL(application->surface));
    return (state & (GDK_TOPLEVEL_STATE_FULLSCREEN |
                     GDK_TOPLEVEL_STATE_MAXIMIZED |
                     GDK_TOPLEVEL_STATE_TILED)) == 0;
}

static int complete_current_size_command(
    boxrig_term_gtk4_application *application,
    uint64_t revision,
    boxrig_ui_size_outcome outcome)
{
    boxrig_ui_layout_result layout;
    boxrig_ui_fixed width;
    boxrig_ui_fixed height;
    bool applied = false;

    if (!widget_allocation_fixed(application, &width, &height) ||
        boxrig_gtk4_terminal_widget_complete_size_command(
            application->widget,
            revision,
            outcome,
            width,
            height,
            &applied,
            &layout) != BOXRIG_UI_OK ||
        !applied) {
        return -1;
    }
    application->pending_size_command = false;
    return 0;
}

int boxrig_term_gtk4_native_host_process_layout_action(
    boxrig_term_gtk4_application *application)
{
    boxrig_ui_layout_action action;
    boxrig_ui_size_command_state command_state;
    boxrig_ui_fixed allocation_width;
    boxrig_ui_fixed allocation_height;
    gint widget_width;
    gint widget_height;
    gint window_width;
    gint window_height;
    gint requested_width;
    gint requested_height;
    gint scale;

    if (application == NULL || application->window == NULL ||
        application->widget == NULL) {
        return -1;
    }
    if (application->surface == NULL ||
        !gtk_widget_get_mapped(GTK_WIDGET(application->widget)) ||
        gtk_widget_get_width(GTK_WIDGET(application->widget)) <= 0 ||
        gtk_widget_get_height(GTK_WIDGET(application->widget)) <= 0) {
        return 0;
    }
    if (boxrig_gtk4_terminal_widget_get_size_command_state(
            application->widget,
            &command_state) != BOXRIG_UI_OK) {
        return -1;
    }
    if (application->pending_size_command) {
        if (command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED) {
            boxrig_ui_size_outcome outcome;

            /* A GdkSurface layout is the asynchronous native configure
             * boundary. Inspect the widget allocation as well because the
             * signal may also represent a relayout without a size change. */
            if (application->surface_layout_generation ==
                application->pending_command_layout_generation) {
                return 0;
            }
            if (!widget_allocation_fixed(
                    application,
                    &allocation_width,
                    &allocation_height)) {
                return -1;
            }
            if (allocation_width == application->pending_command_width &&
                allocation_height == application->pending_command_height) {
                outcome = BOXRIG_UI_SIZE_ACCEPTED;
            } else if (!native_window_is_normal(application)) {
                outcome = BOXRIG_UI_SIZE_CONSTRAINED;
            } else if (gtk_widget_get_width(
                           GTK_WIDGET(application->widget)) !=
                           application->command_start_widget_width ||
                       gtk_widget_get_height(
                           GTK_WIDGET(application->widget)) !=
                           application->command_start_widget_height) {
                outcome = BOXRIG_UI_SIZE_SUPERSEDED;
            } else {
                outcome = BOXRIG_UI_SIZE_REJECTED;
            }
            return complete_current_size_command(
                application,
                application->pending_command_revision,
                outcome);
        }
        application->pending_size_command = false;
    }
    if (!boxrig_gtk4_terminal_widget_take_layout_action(
            application->widget, &action)) {
        return 0;
    }
    if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        if (application->session == NULL ||
            boxrig_session_resize(
                application->session,
                action.value.grid_resize.rows,
                action.value.grid_resize.columns) != BOXRIG_SESSION_OK) {
            (void)boxrig_gtk4_terminal_widget_complete_grid_resize(
                application->widget, action.revision, false);
            return -1;
        }
        return boxrig_gtk4_terminal_widget_complete_grid_resize(
                   application->widget, action.revision, true) == BOXRIG_UI_OK
                   ? 0
                   : -1;
    }
    if (!native_window_is_normal(application)) {
        return complete_current_size_command(
            application,
            action.revision,
            BOXRIG_UI_SIZE_CONSTRAINED);
    }
    widget_width = gtk_widget_get_width(GTK_WIDGET(application->widget));
    widget_height = gtk_widget_get_height(GTK_WIDGET(application->widget));
    gtk_window_get_default_size(
        application->window,
        &window_width,
        &window_height);
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(application->widget));
    if (widget_width <= 0 || widget_height <= 0 || window_width <= 0 ||
        window_height <= 0 || window_width < widget_width ||
        window_height < widget_height ||
        !fixed_to_logical_pixels(
            action.value.content_size.width, scale, &requested_width) ||
        !fixed_to_logical_pixels(
            action.value.content_size.height, scale, &requested_height) ||
        requested_width >
            G_MAXINT - (window_width - widget_width) ||
        requested_height >
            G_MAXINT - (window_height - widget_height)) {
        return -1;
    }
    requested_width += window_width - widget_width;
    requested_height += window_height - widget_height;
    /* The portable command covers only terminal content. Preserve the current
     * content chrome extent in the one native size operation. GtkWindow adds
     * client-side decoration to its default size, so using the GdkSurface
     * extent here would count that decoration twice. */
    application->pending_size_command = true;
    application->pending_command_revision = action.revision;
    application->pending_command_layout_generation =
        application->surface_layout_generation;
    application->pending_command_width = action.value.content_size.width;
    application->pending_command_height = action.value.content_size.height;
    application->command_start_widget_width = widget_width;
    application->command_start_widget_height = widget_height;
    gtk_window_set_default_size(
        application->window,
        requested_width,
        requested_height);
    return 0;
}

static bool valid_text_payload(const uint8_t *bytes, size_t length)
{
    const char *text = length == 0 ? "" : (const char *)bytes;

    return length <= (size_t)G_MAXSSIZE &&
           (length == 0 || bytes != NULL) &&
           (length == 0 || memchr(bytes, '\0', length) == NULL) &&
           g_utf8_validate(text, (gssize)length, NULL);
}

static int push_window_title(boxrig_term_gtk4_application *application)
{
    const char *current = gtk_window_get_title(application->window);
    char *copy = g_strdup(current == NULL ? "" : current);

    if (copy == NULL) {
        return -1;
    }
    if (application->window_title_count ==
        BOXRIG_TERM_GTK4_TITLE_STACK_CAPACITY) {
        g_free(application->window_titles[0]);
        memmove(
            application->window_titles,
            application->window_titles + 1,
            (BOXRIG_TERM_GTK4_TITLE_STACK_CAPACITY - 1) *
                sizeof(application->window_titles[0]));
        application->window_title_count--;
    }
    application->window_titles[application->window_title_count] = copy;
    application->window_title_count++;
    return 0;
}

static void pop_window_title(boxrig_term_gtk4_application *application)
{
    char *title;

    if (application->window_title_count == 0) {
        return;
    }
    application->window_title_count--;
    title = application->window_titles[application->window_title_count];
    application->window_titles[application->window_title_count] = NULL;
    gtk_window_set_title(application->window, title);
    g_free(title);
}

static int apply_title(
    boxrig_term_gtk4_application *application,
    const boxrig_host_item *item)
{
    uint32_t operation = item->flags &
                         (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP);
    char *title;

    if (!application->config->host.allow_title_change) {
        return 0;
    }
    if (operation ==
        (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP)) {
        return -1;
    }
    if ((item->flags & BOXRIG_TITLE_WINDOW) == 0) {
        return 0;
    }
    if (operation == BOXRIG_TITLE_STACK_PUSH) {
        return push_window_title(application);
    }
    if (operation == BOXRIG_TITLE_STACK_POP) {
        pop_window_title(application);
        return 0;
    }
    if (!valid_text_payload(item->payload, item->payload_length)) {
        return -1;
    }
    title = g_strndup((const char *)item->payload, item->payload_length);
    if (title == NULL) {
        return -1;
    }
    gtk_window_set_title(application->window, title);
    g_free(title);
    return 0;
}

static bool publish_clipboard(
    GdkClipboard *clipboard,
    const uint8_t *bytes,
    size_t length)
{
    GBytes *content;
    GdkContentProvider *provider;
    bool published;

    if (clipboard == NULL) {
        return false;
    }
    content = g_bytes_new(bytes, length);
    if (content == NULL) {
        return false;
    }
    provider = gdk_content_provider_new_for_bytes(
        "text/plain;charset=utf-8",
        content);
    if (provider == NULL) {
        g_bytes_unref(content);
        return false;
    }
    published = gdk_clipboard_set_content(clipboard, provider);
    g_object_unref(provider);
    g_bytes_unref(content);
    return published;
}

static boxrig_host_request_result write_clipboard(
    boxrig_term_gtk4_application *application,
    const boxrig_host_item *item)
{
    const uint32_t supported =
        BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD |
        BOXRIG_CLIPBOARD_SELECTION_PRIMARY;
    GdkDisplay *display;

    if (!application->config->host.allow_clipboard_write) {
        return BOXRIG_HOST_REQUEST_DENIED;
    }
    if (item->flags == 0 || (item->flags & ~supported) != 0 ||
        !valid_text_payload(item->payload, item->payload_length)) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    display = gtk_widget_get_display(GTK_WIDGET(application->widget));
    if ((item->flags & BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD) != 0 &&
        !publish_clipboard(
            gdk_display_get_clipboard(display),
            item->payload,
            item->payload_length)) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    if ((item->flags & BOXRIG_CLIPBOARD_SELECTION_PRIMARY) != 0 &&
        !publish_clipboard(
            gdk_display_get_primary_clipboard(display),
            item->payload,
            item->payload_length)) {
        return BOXRIG_HOST_REQUEST_UNAVAILABLE;
    }
    return BOXRIG_HOST_REQUEST_SUCCESS;
}

static int consume_and_complete(
    boxrig_term_gtk4_application *application,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *bytes,
    size_t length)
{
    if (boxrig_ui_host_item_consumer_consume(application->host_consumer) !=
            BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_complete(
            application->host_consumer,
            request_id,
            result,
            bytes,
            length) != BOXRIG_UI_OK) {
        return -1;
    }
    return 0;
}

int boxrig_term_gtk4_process_host_items(
    boxrig_term_gtk4_application *application,
    bool *pending_out)
{
    boxrig_host_item item;
    size_t count = 0;

    if (application == NULL || application->host_consumer == NULL ||
        pending_out == NULL) {
        return -1;
    }
    bool focused = gtk_window_is_active(application->window) &&
        gtk_widget_has_focus(GTK_WIDGET(application->widget));
    bool visible = gtk_widget_get_mapped(GTK_WIDGET(application->window));
    boxrig_freedesktop_notifications_set_window_state(
        application->notifications, focused, visible);
    *pending_out = false;
    while (count < BOXRIG_TERM_GTK4_HOST_ITEM_BATCH_CAPACITY &&
           boxrig_ui_host_item_consumer_peek(
               application->host_consumer,
               &item)) {
        count++;
        if (item.type == BOXRIG_HOST_ITEM_BELL) {
            gtk_widget_error_bell(GTK_WIDGET(application->widget));
        } else if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (apply_title(application, &item) == -1) {
                return -1;
            }
        } else if (item.type == BOXRIG_HOST_ITEM_WORKING_DIRECTORY) {
            /* The untrusted URI remains metadata and causes no host action. */
        } else if (item.type == BOXRIG_HOST_ITEM_GRID_RESIZE) {
            /* The sample keeps remote sizing separate from user layout. */
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE) {
            boxrig_host_request_result result =
                write_clipboard(application, &item);

            if (consume_and_complete(
                    application,
                    item.request_id,
                    result, NULL, 0) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ) {
            boxrig_host_request_result result =
                application->config->host.allow_clipboard_read
                    ? BOXRIG_HOST_REQUEST_UNAVAILABLE
                    : BOXRIG_HOST_REQUEST_DENIED;

            if (consume_and_complete(
                    application,
                    item.request_id,
                    result, NULL, 0) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_NOTIFICATION) {
            if (boxrig_freedesktop_notifications_process(
                    application->notifications, focused, visible) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
            uint8_t *bytes = NULL;
            size_t length = 0;
            boxrig_host_request_result result = boxrig_posix_read_graphics_transport(
                item.graphics_transport,
                application->config->host.allow_graphics_external,
                &bytes,
                &length);
            int completion = consume_and_complete(
                application, item.request_id, result, bytes, length);
            free(bytes);
            if (completion == -1) {
                return -1;
            }
            continue;
        } else {
            return -1;
        }
        if (boxrig_ui_host_item_consumer_consume(
                application->host_consumer) != BOXRIG_UI_OK) {
            return -1;
        }
    }
    *pending_out = boxrig_ui_host_item_consumer_peek(
        application->host_consumer,
        &item);
    return 0;
}

static int apply_background_opacity_shortcut(
    boxrig_term_gtk4_application *application,
    boxrig_ui_background_opacity_shortcut shortcut)
{
    boxrig_ui_transparency_state state;
    boxrig_ui_transparency_config config;
    uint8_t next_opacity;
    uint8_t next_remembered_opacity;

    if (boxrig_gtk4_terminal_widget_get_transparency(
            application->widget,
            &state) != BOXRIG_UI_OK) {
        return -1;
    }
    if (!boxrig_ui_background_opacity_shortcut_next(
            shortcut,
            state.config.background_opacity,
            application->remembered_background_opacity,
            &next_opacity,
            &next_remembered_opacity)) {
        return 0;
    }
    config = state.config;
    config.background_opacity = next_opacity;
    if (boxrig_gtk4_terminal_widget_set_transparency(
            application->widget,
            &config) != BOXRIG_UI_OK) {
        return -1;
    }
    application->remembered_background_opacity = next_remembered_opacity;
    boxrig_term_gtk4_request_dispatch(application);
    return 0;
}

static bool view_action_status_consumes_input(boxrig_ui_status status)
{
    switch (status) {
    case BOXRIG_UI_OK:
    case BOXRIG_UI_OUT_OF_MEMORY:
    case BOXRIG_UI_LIMIT_EXCEEDED:
    case BOXRIG_UI_UNAVAILABLE:
    case BOXRIG_UI_BUSY:
    case BOXRIG_UI_STALE:
        return true;
    case BOXRIG_UI_INVALID_ARGUMENT:
    default:
        return false;
    }
}

static boxrig_ui_status apply_view_action(
    boxrig_term_gtk4_application *application,
    const boxrig_ui_view_action *action)
{
    if (application == NULL || application->widget == NULL ||
        action == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return boxrig_gtk4_terminal_widget_apply_view_action(
        application->widget,
        action);
}

static bool update_hyperlink_hover(
    boxrig_term_gtk4_application *application,
    const boxrig_ui_hit_test_result *hit)
{
    boxrig_term_text_pointer_target target;

    if (hit == NULL || application->text_policy == NULL) {
        return false;
    }
    /* A queued motion can outlive its allocation or annotations. Ignore that
     * stale hit without retargeting it to newer content. */
    return boxrig_term_text_policy_update_pointer_hover(
               application->text_policy,
               hit,
               &target) == 0 || errno == EAGAIN;
}

static bool queue_zoom_shortcut(
    boxrig_term_gtk4_application *application,
    boxrig_ui_zoom_shortcut zoom_shortcut)
{
    if (application->zoom_shortcut_count >=
        BOXRIG_TERM_GTK4_ZOOM_SHORTCUT_BATCH_CAPACITY) {
        return false;
    }
    /* Input routers run inside widget frame(), where font transactions are
     * deliberately rejected as reentrant calls. */
    application->zoom_shortcuts[application->zoom_shortcut_count++] =
        zoom_shortcut;
    boxrig_term_gtk4_request_dispatch(application);
    return true;
}


static void book_link_deadline(boxrig_term_gtk4_application *application)
{
    uint64_t deadline;
    if (application->link_source == NULL || g_source_is_destroyed(application->link_source)) return;
    gint64 due = -1;
    if (boxrig_term_link_gesture_deadline(application->link_gesture, &deadline))
        due = deadline > (uint64_t)G_MAXINT64 / 1000 ? G_MAXINT64 : (gint64)deadline * 1000;
    g_source_set_ready_time(application->link_source, due);
}

static void cancel_link(boxrig_term_gtk4_application *application)
{
    boxrig_term_link_gesture_cancel(application->link_gesture);
    book_link_deadline(application);
}

static void open_link(void *context, const char *uri, size_t length)
{
    boxrig_term_gtk4_application *application = context;
    (void)boxrig_term_uri_launcher_open(application->uri_launcher, uri, length);
}

static gboolean dispatch_links(gpointer context)
{
    boxrig_term_gtk4_application *application = context;
    if (application->failed || application->quitting) return G_SOURCE_REMOVE;
    int changed = boxrig_term_gtk4_update_pointer(application);
    if (changed < 0 || boxrig_term_link_gesture_dispatch(application->link_gesture,
            (uint64_t)(g_get_monotonic_time() / 1000)) != 0) {
        boxrig_term_gtk4_fail(application);
        return G_SOURCE_REMOVE;
    }
    if (changed != 0) boxrig_term_gtk4_request_dispatch(application);
    book_link_deadline(application);
    return G_SOURCE_CONTINUE;
}

static void link_focus_changed(GtkWidget *widget, GParamSpec *property, gpointer context)
{
    (void)property;
    if (!gtk_widget_has_focus(widget)) cancel_link(context);
}

static void link_unmapped(GtkWidget *widget, gpointer context)
{
    (void)widget;
    cancel_link(context);
}

int boxrig_term_gtk4_start_links(boxrig_term_gtk4_application *application)
{
    if (boxrig_term_uri_launcher_create(stderr, &application->uri_launcher) != 0 ||
        boxrig_term_link_gesture_create(application->terminal,
            boxrig_gtk4_terminal_widget_view(application->widget),
            application->text_policy, open_link, application, &application->link_gesture) != 0)
        return -1;
    application->link_source = boxrig_term_uri_launcher_source_new(application->uri_launcher);
    if (application->link_source == NULL) return -1;
    g_source_set_callback(application->link_source, dispatch_links, application, NULL);
    if (g_source_attach(application->link_source, NULL) == 0) return -1;
    g_signal_connect(application->widget, "notify::has-focus", G_CALLBACK(link_focus_changed), application);
    g_signal_connect(application->widget, "unmap", G_CALLBACK(link_unmapped), application);
    return 0;
}

void boxrig_term_gtk4_stop_links(boxrig_term_gtk4_application *application)
{
    if (application->link_source != NULL) {
        g_source_destroy(application->link_source);
        g_source_unref(application->link_source);
        application->link_source = NULL;
    }
    boxrig_term_link_gesture_destroy(application->link_gesture);
    application->link_gesture = NULL;
    boxrig_term_uri_launcher_destroy(application->uri_launcher);
    application->uri_launcher = NULL;
}

boxrig_input_mouse_route boxrig_term_gtk4_route_application_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context)
{
    boxrig_term_gtk4_application *application = context;
    boxrig_input_mouse_route selection_route;
    boxrig_ui_view_action action;
    boxrig_ui_view *view;
    boxrig_ui_status status;
    bool override;
    bool selection_completed;

    if (application == NULL || application->widget == NULL ||
        application->terminal == NULL || event == NULL || hit == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    view = boxrig_gtk4_terminal_widget_view(application->widget);
    if (view == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    double pointer_scale = gtk_widget_get_scale_factor(GTK_WIDGET(application->widget));
    application->pointer_x = (double)hit->x / BOXRIG_UI_FIXED_ONE / pointer_scale;
    application->pointer_y = (double)hit->y / BOXRIG_UI_FIXED_ONE / pointer_scale;
    application->pointer_inside = hit->inside_grid;
    gint interval = 400, drag_pixels = 8, click_pixels = 5;
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(application->widget)),
        "gtk-double-click-time", &interval, "gtk-double-click-distance", &click_pixels,
        "gtk-dnd-drag-threshold", &drag_pixels, NULL);
    boxrig_ui_fixed drag_distance = (boxrig_ui_fixed)drag_pixels * BOXRIG_UI_FIXED_ONE;
    gint scale = gtk_widget_get_scale_factor(GTK_WIDGET(application->widget));
    drag_distance = drag_distance > INT64_MAX / scale ? INT64_MAX : drag_distance * scale;
    boxrig_ui_fixed click_distance = (boxrig_ui_fixed)click_pixels * BOXRIG_UI_FIXED_ONE;
    click_distance = click_distance > INT64_MAX / scale ? INT64_MAX : click_distance * scale;
    boxrig_input_mouse_route link_route = boxrig_term_link_gesture_route(
        application->link_gesture, &application->selection_drag_active, event, hit,
        (uint64_t)(g_get_monotonic_time() / 1000), (uint32_t)interval, click_distance, drag_distance);
    book_link_deadline(application);
    if (link_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) return link_route;
    bool copy_requested;
    boxrig_ui_status copy_status;
    boxrig_input_mouse_route copy_route = boxrig_term_mouse_policy_route_copy(
        view, application->selection_drag_active,
        &application->copy_button_active, event, hit, &copy_requested,
        &copy_status);
    if (copy_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        if (copy_requested) {
            boxrig_ui_view_action action;
            action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
            action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
            copy_status = apply_view_action(application, &action);
        }
        if (copy_status != BOXRIG_UI_OK) {
            fprintf(stderr,
                "boxrig_term: could not copy selection (status=%d).\n",
                (int)copy_status);
        }
        return copy_route;
    }
    if (application->selection_drag_active) {
        selection_route = boxrig_term_mouse_policy_route_selection(
            application->terminal,
            view,
            &application->selection_drag_active,
            event,
            hit,
            &selection_completed);
        if (selection_route == BOXRIG_INPUT_MOUSE_ROUTE_ERROR) {
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        if (selection_completed) {
            action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
            action.clipboard_target =
                BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
            status = apply_view_action(application, &action);
            if (!view_action_status_consumes_input(status)) {
                return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
            }
        }
        return selection_route;
    }
    boxrig_ui_zoom_shortcut wheel_zoom;
    boxrig_input_mouse_route wheel_route =
        boxrig_term_mouse_policy_route_wheel(
            application->terminal,
            false,
            event,
            hit,
            &wheel_zoom);
    if (wheel_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        if (wheel_route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION &&
            wheel_zoom != BOXRIG_UI_ZOOM_SHORTCUT_NONE &&
            !queue_zoom_shortcut(application, wheel_zoom)) {
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        return wheel_route;
    }
    if (event->action == BOXRIG_MOUSE_MOTION) {
        return update_hyperlink_hover(application, hit)
                   ? BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL
                   : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->action != BOXRIG_MOUSE_PRESS) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    override = (event->modifiers & BOXRIG_MODIFIER_SHIFT) != 0;
    if (event->button == BOXRIG_MOUSE_BUTTON_MIDDLE) {
        if (boxrig_terminal_mouse_reporting_enabled(application->terminal) &&
            !override) {
            return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
        }
        action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target =
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
        status = apply_view_action(application, &action);
        return view_action_status_consumes_input(status)
                   ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
                   : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->button != BOXRIG_MOUSE_BUTTON_LEFT) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    return boxrig_term_mouse_policy_route_selection(
        application->terminal,
        view,
        &application->selection_drag_active,
        event,
        hit,
        &selection_completed);
}

boxrig_input_key_route boxrig_term_gtk4_route_application_shortcut(
    const boxrig_key_event *event,
    void *context)
{
    boxrig_term_gtk4_application *application = context;
    boxrig_ui_background_opacity_shortcut opacity_shortcut =
        boxrig_ui_background_opacity_shortcut_match(event);
    boxrig_ui_clipboard_shortcut clipboard_shortcut =
        boxrig_ui_clipboard_shortcut_match(event);
    boxrig_ui_zoom_shortcut zoom_shortcut =
        boxrig_ui_zoom_shortcut_match(event);
    boxrig_ui_view_action action;
    boxrig_ui_status status;

    if (application == NULL || application->widget == NULL) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    boxrig_term_posix_runtime_prepare_key(application->runtime, event);
    const char *renderer_demo = getenv("BOXRIG_RENDERER_DEMO");
    if (renderer_demo != NULL && strcmp(renderer_demo, "1") == 0 &&
        (event->key == BOXRIG_KEY_F6 || event->key == BOXRIG_KEY_F7 || event->key == BOXRIG_KEY_F8) &&
        (event->modifiers & ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK)) ==
            (BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT)) {
        if (event->action == BOXRIG_KEY_PRESS) {
            /* Input routing runs inside frame(); apply the request after it returns. */
            application->pending_render_policy = event->key == BOXRIG_KEY_F6
                ? BOXRIG_GTK_RENDER_CAIRO : event->key == BOXRIG_KEY_F7
                ? BOXRIG_GTK_RENDER_OPENGL_PREFERRED : BOXRIG_GTK_RENDER_OPENGL_REQUIRED;
            application->render_policy_pending = true;
            boxrig_term_gtk4_request_dispatch(application);
        }
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    boxrig_input_key_route scrollbar_route = boxrig_term_scrollbar_demo_route_key(
        &application->scrollbar_demo, event);
    if (scrollbar_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return scrollbar_route;
    boxrig_input_key_route decoration_route = boxrig_term_decoration_demo_route_key(
        boxrig_gtk4_terminal_widget_view(application->widget), event);
    if (decoration_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return decoration_route;
    boxrig_input_key_route font_route =
        boxrig_term_font_demo_route_key(&application->font_demo, event);
    if (font_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return font_route;
    boxrig_input_key_route color_route = boxrig_term_color_demo_route_key(
        &application->color_demo, boxrig_gtk4_terminal_widget_view(application->widget), event);
    if (color_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return color_route;
    if (clipboard_shortcut != BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE) {
        action.kind =
            clipboard_shortcut == BOXRIG_UI_CLIPBOARD_SHORTCUT_COPY
                ? BOXRIG_UI_VIEW_ACTION_COPY
                : BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        status = apply_view_action(application, &action);
        return view_action_status_consumes_input(status)
                   ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
                   : BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    if (application->enable_background_opacity &&
        opacity_shortcut != BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE) {
        if (event->action != BOXRIG_KEY_RELEASE &&
            apply_background_opacity_shortcut(
                application,
                opacity_shortcut) == -1) {
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
        }
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    if (zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_NONE) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (event->action == BOXRIG_KEY_RELEASE) {
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    return queue_zoom_shortcut(application, zoom_shortcut)
               ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
               : BOXRIG_INPUT_KEY_ROUTE_ERROR;
}

int boxrig_term_gtk4_apply_pending_zoom_shortcuts(
    boxrig_term_gtk4_application *application)
{
    size_t count;
    size_t index;
    bool any_changed = false;

    if (application == NULL || application->widget == NULL) {
        return -1;
    }
    count = application->zoom_shortcut_count;
    application->zoom_shortcut_count = 0;
    for (index = 0; index < count; index++) {
        boxrig_ui_zoom_shortcut shortcut =
            application->zoom_shortcuts[index];
        boxrig_ui_status status;
        bool changed = false;

        if (shortcut == BOXRIG_UI_ZOOM_SHORTCUT_RESET) {
            status = boxrig_gtk4_terminal_widget_visual_zoom_reset(
                application->widget,
                &changed);
        } else {
            boxrig_font_zoom_direction direction =
                shortcut == BOXRIG_UI_ZOOM_SHORTCUT_IN
                    ? BOXRIG_FONT_ZOOM_INCREASE
                    : BOXRIG_FONT_ZOOM_DECREASE;

            if (shortcut != BOXRIG_UI_ZOOM_SHORTCUT_IN &&
                shortcut != BOXRIG_UI_ZOOM_SHORTCUT_OUT) {
                return -1;
            }
            status = boxrig_gtk4_terminal_widget_visual_zoom(
                application->widget,
                direction,
                boxrig_font_visual_step_policy_default(),
                &changed);
        }
        if (status != BOXRIG_UI_OK) {
            return -1;
        }
        any_changed = any_changed || changed;
    }
    if (any_changed) {
        boxrig_term_gtk4_request_dispatch(application);
    }
    return 0;
}

void boxrig_term_gtk4_native_host_destroy(
    boxrig_term_gtk4_application *application)
{
    size_t index;

    if (application == NULL) {
        return;
    }
    if (application->surface != NULL &&
        application->surface_state_handler != 0) {
        g_signal_handler_disconnect(
            application->surface,
            application->surface_state_handler);
    }
    if (application->surface != NULL &&
        application->surface_layout_handler != 0) {
        g_signal_handler_disconnect(
            application->surface,
            application->surface_layout_handler);
    }
    g_clear_object(&application->surface);
    application->surface_state_handler = 0;
    application->surface_layout_handler = 0;
    application->pending_size_command = false;
    for (index = 0; index < application->window_title_count; index++) {
        g_free(application->window_titles[index]);
        application->window_titles[index] = NULL;
    }
    application->window_title_count = 0;
}

static void pointer_entered(GtkEventControllerMotion *controller, double x, double y, gpointer context)
{
    boxrig_term_gtk4_application *application = context;
    (void)controller;
    application->pointer_inside = true;
    application->pointer_x = x;
    application->pointer_y = y;
    boxrig_term_gtk4_request_dispatch(application);
}

static void pointer_left(GtkEventControllerMotion *controller, gpointer context)
{
    boxrig_term_gtk4_application *application = context;
    (void)controller;
    application->pointer_inside = false;
    cancel_link(application);
    boxrig_term_gtk4_request_dispatch(application);
}

void boxrig_term_gtk4_observe_pointer(boxrig_term_gtk4_application *application)
{
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(pointer_entered), application);
    g_signal_connect(motion, "motion", G_CALLBACK(pointer_entered), application);
    g_signal_connect(motion, "leave", G_CALLBACK(pointer_left), application);
    gtk_widget_add_controller(GTK_WIDGET(application->widget), motion);
}

int boxrig_term_gtk4_update_pointer(boxrig_term_gtk4_application *application)
{
    if (application->text_policy == NULL || application->widget == NULL) return 0;
    GtkWidget *widget = GTK_WIDGET(application->widget);
    double scale = gtk_widget_get_scale_factor(widget);
    double x = application->pointer_x * scale * BOXRIG_UI_FIXED_ONE;
    double y = application->pointer_y * scale * BOXRIG_UI_FIXED_ONE;
    bool inside = application->pointer_inside && gtk_widget_get_mapped(widget) &&
        boxrig_ui_view_focused(boxrig_gtk4_terminal_widget_view(application->widget)) &&
        isfinite(x) && isfinite(y) && x >= (double)INT64_MIN && x < (double)INT64_MAX &&
        y >= (double)INT64_MIN && y < (double)INT64_MAX;
    if (!inside) cancel_link(application);
    bool hand;
    int changed = boxrig_term_text_policy_refresh_pointer(application->text_policy,
        inside, inside ? (boxrig_ui_fixed)x : 0, inside ? (boxrig_ui_fixed)y : 0, &hand);
    if (changed < 0) return -1;
    if (!application->pointer_initialized || hand != application->pointer_hand) {
        gtk_widget_set_cursor_from_name(widget, hand ? "pointer" : "text");
        application->pointer_hand = hand;
        application->pointer_initialized = true;
    }
    return changed;
}
