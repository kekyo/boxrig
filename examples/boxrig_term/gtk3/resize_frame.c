/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "resize_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum {
    RESIZE_BORDER = 6,
    RESIZE_HANDLE_COUNT = 8
};

typedef struct resize_handle_config {
    GdkWindowEdge edge;
    const char *cursor_name;
    const char *accessible_id;
} resize_handle_config;

struct boxrig_term_gtk3_resize_frame {
    GtkWindow *window;
    GtkWidget *titlebar;
    GtkWidget *content;
    GtkWidget *titlebar_root;
    GtkWidget *content_root;
    GtkWidget *handles[RESIZE_HANDLE_COUNT];
    GdkWindowState window_state;
    gulong window_state_handler;
    uint8_t background_opacity;
};

static const resize_handle_config handle_configs[RESIZE_HANDLE_COUNT] = {
    {GDK_WINDOW_EDGE_NORTH_WEST,
     "nw-resize",
     "boxrig-resize-northwest"},
    {GDK_WINDOW_EDGE_NORTH, "n-resize", "boxrig-resize-north"},
    {GDK_WINDOW_EDGE_NORTH_EAST,
     "ne-resize",
     "boxrig-resize-northeast"},
    {GDK_WINDOW_EDGE_WEST, "w-resize", "boxrig-resize-west"},
    {GDK_WINDOW_EDGE_EAST, "e-resize", "boxrig-resize-east"},
    {GDK_WINDOW_EDGE_SOUTH_WEST,
     "sw-resize",
     "boxrig-resize-southwest"},
    {GDK_WINDOW_EDGE_SOUTH, "s-resize", "boxrig-resize-south"},
    {GDK_WINDOW_EDGE_SOUTH_EAST,
     "se-resize",
     "boxrig-resize-southeast"}};

static bool edge_uses_top(GdkWindowEdge edge)
{
    return edge == GDK_WINDOW_EDGE_NORTH_WEST ||
           edge == GDK_WINDOW_EDGE_NORTH ||
           edge == GDK_WINDOW_EDGE_NORTH_EAST;
}

static bool edge_uses_right(GdkWindowEdge edge)
{
    return edge == GDK_WINDOW_EDGE_NORTH_EAST ||
           edge == GDK_WINDOW_EDGE_EAST ||
           edge == GDK_WINDOW_EDGE_SOUTH_EAST;
}

static bool edge_uses_bottom(GdkWindowEdge edge)
{
    return edge == GDK_WINDOW_EDGE_SOUTH_WEST ||
           edge == GDK_WINDOW_EDGE_SOUTH ||
           edge == GDK_WINDOW_EDGE_SOUTH_EAST;
}

static bool edge_uses_left(GdkWindowEdge edge)
{
    return edge == GDK_WINDOW_EDGE_NORTH_WEST ||
           edge == GDK_WINDOW_EDGE_WEST ||
           edge == GDK_WINDOW_EDGE_SOUTH_WEST;
}

static bool edge_is_enabled(
    const boxrig_term_gtk3_resize_frame *frame,
    GdkWindowEdge edge)
{
    GdkWindowState state = frame->window_state;
    GdkWindowState tiled =
        GDK_WINDOW_STATE_TILED | GDK_WINDOW_STATE_TOP_TILED |
        GDK_WINDOW_STATE_RIGHT_TILED | GDK_WINDOW_STATE_BOTTOM_TILED |
        GDK_WINDOW_STATE_LEFT_TILED;

    if (!gtk_window_get_resizable(frame->window) ||
        (state & (GDK_WINDOW_STATE_MAXIMIZED |
                  GDK_WINDOW_STATE_FULLSCREEN)) != 0) {
        return false;
    }
    if ((state & tiled) == 0) {
        return true;
    }
    return (!edge_uses_top(edge) ||
            (state & GDK_WINDOW_STATE_TOP_RESIZABLE) != 0) &&
           (!edge_uses_right(edge) ||
            (state & GDK_WINDOW_STATE_RIGHT_RESIZABLE) != 0) &&
           (!edge_uses_bottom(edge) ||
            (state & GDK_WINDOW_STATE_BOTTOM_RESIZABLE) != 0) &&
           (!edge_uses_left(edge) ||
            (state & GDK_WINDOW_STATE_LEFT_RESIZABLE) != 0);
}

static void update_handle_cursor(
    boxrig_term_gtk3_resize_frame *frame,
    size_t index)
{
    GtkWidget *handle = frame->handles[index];
    GdkWindow *parent = gtk_widget_get_window(handle);
    GList *surfaces;
    GList *item;
    GdkCursor *cursor = NULL;

    if (parent == NULL) {
        return;
    }
    if (edge_is_enabled(frame, handle_configs[index].edge)) {
        cursor = gdk_cursor_new_from_name(
            gtk_widget_get_display(handle),
            handle_configs[index].cursor_name);
    }
    /* Invisible event boxes share the drawing window, but GTK registers a
     * separate input-only child for each handle. Keep its cursor local. */
    surfaces = gdk_window_get_children_with_user_data(parent, handle);
    for (item = surfaces; item != NULL; item = item->next) {
        gdk_window_set_cursor(item->data, cursor);
    }
    g_list_free(surfaces);
    g_clear_object(&cursor);
}

static void update_cursors(boxrig_term_gtk3_resize_frame *frame)
{
    size_t index;

    for (index = 0; index < RESIZE_HANDLE_COUNT; index++) {
        update_handle_cursor(frame, index);
    }
}

static size_t handle_index(
    const boxrig_term_gtk3_resize_frame *frame,
    GtkWidget *handle)
{
    size_t index;

    for (index = 0; index < RESIZE_HANDLE_COUNT; index++) {
        if (frame->handles[index] == handle) {
            return index;
        }
    }
    return RESIZE_HANDLE_COUNT;
}

static gboolean handle_button_pressed(
    GtkWidget *handle,
    GdkEventButton *event,
    gpointer user_data)
{
    boxrig_term_gtk3_resize_frame *frame = user_data;
    size_t index = handle_index(frame, handle);

    if (index == RESIZE_HANDLE_COUNT || event->button != 1 ||
        !edge_is_enabled(frame, handle_configs[index].edge)) {
        return FALSE;
    }
    gtk_window_begin_resize_drag(
        frame->window,
        handle_configs[index].edge,
        (gint)event->button,
        (gint)event->x_root,
        (gint)event->y_root,
        event->time);
    return TRUE;
}

static void handle_realized(GtkWidget *handle, gpointer user_data)
{
    boxrig_term_gtk3_resize_frame *frame = user_data;
    size_t index = handle_index(frame, handle);

    if (index != RESIZE_HANDLE_COUNT) {
        update_handle_cursor(frame, index);
    }
}

static gboolean window_state_changed(
    GtkWidget *window,
    GdkEventWindowState *event,
    gpointer user_data)
{
    boxrig_term_gtk3_resize_frame *frame = user_data;

    (void)window;
    frame->window_state = event->new_window_state;
    update_cursors(frame);
    return FALSE;
}

static gboolean content_root_drawn(
    GtkWidget *content_root,
    cairo_t *cairo,
    gpointer user_data)
{
    boxrig_term_gtk3_resize_frame *frame = user_data;
    gint width = gtk_widget_get_allocated_width(content_root);
    gint height = gtk_widget_get_allocated_height(content_root);

    if (width <= 0 || height <= 0) {
        return FALSE;
    }
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(
        cairo,
        0.0,
        0.0,
        0.0,
        (double)frame->background_opacity / 255.0);
    cairo_rectangle(cairo, 0.0, 0.0, RESIZE_BORDER, height);
    cairo_rectangle(
        cairo,
        width - RESIZE_BORDER,
        0.0,
        RESIZE_BORDER,
        height);
    cairo_rectangle(
        cairo,
        RESIZE_BORDER,
        height - RESIZE_BORDER,
        width - RESIZE_BORDER * 2,
        RESIZE_BORDER);
    cairo_fill(cairo);
    cairo_restore(cairo);
    return FALSE;
}

static GtkWidget *create_handle(
    boxrig_term_gtk3_resize_frame *frame,
    size_t index,
    gint width,
    gint height)
{
    GtkWidget *handle = gtk_event_box_new();
    AtkObject *accessible;

    if (handle == NULL) {
        return NULL;
    }
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), FALSE);
    gtk_event_box_set_above_child(GTK_EVENT_BOX(handle), TRUE);
    gtk_widget_set_size_request(handle, width, height);
    gtk_widget_set_name(handle, handle_configs[index].accessible_id);
    gtk_widget_add_events(handle, GDK_BUTTON_PRESS_MASK);
    accessible = gtk_widget_get_accessible(handle);
    if (accessible != NULL &&
        g_object_class_find_property(
            G_OBJECT_GET_CLASS(accessible),
            "accessible-id") != NULL) {
        g_object_set(
            accessible,
            "accessible-id",
            handle_configs[index].accessible_id,
            NULL);
    }
    frame->handles[index] = handle;
    g_signal_connect(
        handle,
        "button-press-event",
        G_CALLBACK(handle_button_pressed),
        frame);
    g_signal_connect(
        handle,
        "realize",
        G_CALLBACK(handle_realized),
        frame);
    return handle;
}

static int create_titlebar_root(boxrig_term_gtk3_resize_frame *frame)
{
    GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *northwest = create_handle(frame, 0, RESIZE_BORDER, RESIZE_BORDER);
    GtkWidget *north = create_handle(frame, 1, -1, RESIZE_BORDER);
    GtkWidget *northeast = create_handle(frame, 2, RESIZE_BORDER, RESIZE_BORDER);

    frame->titlebar_root = gtk_overlay_new();
    if (strip == NULL || northwest == NULL || north == NULL ||
        northeast == NULL || frame->titlebar_root == NULL) {
        return -1;
    }
    g_object_ref_sink(frame->titlebar_root);
    gtk_widget_set_hexpand(north, TRUE);
    gtk_box_pack_start(GTK_BOX(strip), northwest, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(strip), north, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(strip), northeast, FALSE, FALSE, 0);
    gtk_widget_set_halign(strip, GTK_ALIGN_FILL);
    gtk_widget_set_valign(strip, GTK_ALIGN_START);
    gtk_widget_set_hexpand(strip, TRUE);
    gtk_container_add(GTK_CONTAINER(frame->titlebar_root), frame->titlebar);
    gtk_overlay_add_overlay(GTK_OVERLAY(frame->titlebar_root), strip);
    gtk_overlay_set_overlay_pass_through(
        GTK_OVERLAY(frame->titlebar_root), strip, FALSE);
    return 0;
}

static int create_content_root(boxrig_term_gtk3_resize_frame *frame)
{
    GtkWidget *west = create_handle(frame, 3, RESIZE_BORDER, -1);
    GtkWidget *east = create_handle(frame, 4, RESIZE_BORDER, -1);
    GtkWidget *southwest = create_handle(frame, 5, RESIZE_BORDER, RESIZE_BORDER);
    GtkWidget *south = create_handle(frame, 6, -1, RESIZE_BORDER);
    GtkWidget *southeast = create_handle(frame, 7, RESIZE_BORDER, RESIZE_BORDER);

    frame->content_root = gtk_grid_new();
    if (west == NULL || east == NULL || southwest == NULL || south == NULL ||
        southeast == NULL || frame->content_root == NULL) {
        return -1;
    }
    g_object_ref_sink(frame->content_root);
    g_signal_connect(
        frame->content_root,
        "draw",
        G_CALLBACK(content_root_drawn),
        frame);
    gtk_widget_set_vexpand(west, TRUE);
    gtk_widget_set_vexpand(east, TRUE);
    gtk_widget_set_hexpand(frame->content, TRUE);
    gtk_widget_set_vexpand(frame->content, TRUE);
    gtk_widget_set_hexpand(south, TRUE);
    gtk_grid_attach(GTK_GRID(frame->content_root), west, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(frame->content_root), frame->content, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(frame->content_root), east, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(frame->content_root), southwest, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(frame->content_root), south, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(frame->content_root), southeast, 2, 1, 1, 1);
    return 0;
}

int boxrig_term_gtk3_resize_frame_create(
    GtkWindow *window,
    GtkWidget *titlebar,
    GtkWidget *content,
    boxrig_term_gtk3_resize_frame **frame_out)
{
    boxrig_term_gtk3_resize_frame *frame;
    GdkWindow *surface;

    if (!GTK_IS_WINDOW(window) || titlebar == NULL || content == NULL ||
        frame_out == NULL || gtk_widget_get_parent(titlebar) != NULL ||
        gtk_widget_get_parent(content) != NULL) {
        return -1;
    }
    *frame_out = NULL;
    frame = calloc(1, sizeof(*frame));
    if (frame == NULL) {
        return -1;
    }
    frame->window = window;
    frame->titlebar = titlebar;
    frame->content = content;
    if (create_titlebar_root(frame) == -1 ||
        create_content_root(frame) == -1) {
        boxrig_term_gtk3_resize_frame_destroy(frame);
        return -1;
    }
    surface = gtk_widget_get_window(GTK_WIDGET(window));
    if (surface != NULL) {
        frame->window_state = gdk_window_get_state(surface);
    }
    frame->window_state_handler = g_signal_connect(
        window,
        "window-state-event",
        G_CALLBACK(window_state_changed),
        frame);
    *frame_out = frame;
    return 0;
}

GtkWidget *boxrig_term_gtk3_resize_frame_titlebar_root(
    boxrig_term_gtk3_resize_frame *frame)
{
    return frame == NULL ? NULL : frame->titlebar_root;
}

GtkWidget *boxrig_term_gtk3_resize_frame_content_root(
    boxrig_term_gtk3_resize_frame *frame)
{
    return frame == NULL ? NULL : frame->content_root;
}

void boxrig_term_gtk3_resize_frame_set_background_opacity(
    boxrig_term_gtk3_resize_frame *frame,
    uint8_t opacity)
{
    if (frame == NULL) {
        return;
    }
    frame->background_opacity = opacity;
    if (frame->content_root != NULL) {
        gtk_widget_queue_draw(frame->content_root);
    }
}

void boxrig_term_gtk3_resize_frame_destroy(
    boxrig_term_gtk3_resize_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    if (frame->window_state_handler != 0 && frame->window != NULL &&
        g_signal_handler_is_connected(
            frame->window, frame->window_state_handler)) {
        g_signal_handler_disconnect(
            frame->window, frame->window_state_handler);
    }
    if (frame->window != NULL && frame->titlebar_root != NULL &&
        gtk_window_get_titlebar(frame->window) == frame->titlebar_root) {
        gtk_window_set_titlebar(frame->window, NULL);
    }
    if (frame->content_root != NULL &&
        gtk_widget_get_parent(frame->content_root) ==
            GTK_WIDGET(frame->window)) {
        gtk_container_remove(
            GTK_CONTAINER(frame->window), frame->content_root);
    }
    if (frame->titlebar_root != NULL && frame->titlebar != NULL &&
        gtk_widget_get_parent(frame->titlebar) == frame->titlebar_root) {
        gtk_container_remove(
            GTK_CONTAINER(frame->titlebar_root), frame->titlebar);
    }
    if (frame->content_root != NULL && frame->content != NULL &&
        gtk_widget_get_parent(frame->content) == frame->content_root) {
        gtk_container_remove(
            GTK_CONTAINER(frame->content_root), frame->content);
    }
    g_clear_object(&frame->titlebar_root);
    g_clear_object(&frame->content_root);
    free(frame);
}
