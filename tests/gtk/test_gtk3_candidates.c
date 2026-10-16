/* boxrig - Under MIT. */
#include "boxrig/gtk3.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL GTK 3 candidates %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

static BoxrigGtk3TerminalWidget *widget;
static boxrig_terminal *terminal;
static IBusBus *bus;
static bool dispatch_requested;
static const char *output_directory;
static size_t mouse_routed;

static boxrig_input_mouse_route route_mouse(const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit, void *context)
{
    (void)event; (void)hit; (void)context;
    mouse_routed++;
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static void request_frame(void *data)
{
    (void)data;
    dispatch_requested = true;
    g_main_context_wakeup(NULL);
}

static gboolean deadline(gpointer data)
{
    fprintf(stderr, "FAIL GTK 3 candidates: no completion for %s\n", (char *)data);
    abort();
}

static void frame(void)
{
    boxrig_ui_component_frame_result result;
    dispatch_requested = false;
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &result) == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (boxrig_gtk3_terminal_widget_take_layout_action(widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(widget, action.revision, true) == BOXRIG_UI_OK);
        result.additional_dispatch = true;
    }
    dispatch_requested |= result.additional_dispatch;
}

static void iterate(void)
{
    gboolean block = !dispatch_requested;
    dispatch_requested = false;
    g_main_context_iteration(NULL, block);
}

static void focused(bool active)
{
    GdkEvent *event = gdk_event_new(GDK_FOCUS_CHANGE);
    event->focus_change.in = active;
    event->focus_change.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget)));
    gboolean handled;
    g_signal_emit_by_name(widget, active ? "focus-in-event" : "focus-out-event", event, &handled);
    gdk_event_free(event);
}

static void client_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    CHECK(reply != NULL && error == NULL);
    g_variant_unref(reply);
    *(bool *)data = true;
}

static void await_client(void)
{
    bool ready = false;
    g_dbus_connection_call(ibus_bus_get_connection(bus), "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe",
        "org.boxrig.IMEProbe", "WaitForClient", g_variant_new("(s)", "boxrig-gtk3"),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, client_ready, &ready);
    guint timeout = g_timeout_add_seconds(60, deadline, "input context focus");
    while (!ready) { frame(); if (!ready) iterate(); }
    g_source_remove(timeout);
}

static void settle(void)
{
    /* The private input FIFO and asynchronous key replies provide completion;
     * elapsed time is never evidence that a candidate was rendered. */
    guint timeout = g_timeout_add_seconds(60, deadline, "completed candidate input");
    for (;;) {
        frame();
        boxrig_gtk_ime_state ime;
        boxrig_ui_component_diagnostic state;
        CHECK(boxrig_gtk3_terminal_widget_get_ime_state(widget, &ime) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &state) == BOXRIG_UI_OK);
        if (ime.direct.pending == 0 && state.native_input_queue_depth == 0 &&
            boxrig_gtk3_terminal_widget_has_presentation(widget)) break;
        iterate();
    }
    g_source_remove(timeout);
}

static void key(guint symbol, guint code)
{
    for (unsigned int release = 0; release < 2; release++) {
        GdkEvent *event = gdk_event_new(release ? GDK_KEY_RELEASE : GDK_KEY_PRESS);
        event->key.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget)));
        event->key.keyval = symbol;
        event->key.hardware_keycode = code;
        GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(widget)));
        CHECK(seat != NULL && gdk_seat_get_keyboard(seat) != NULL);
        gdk_event_set_device(event, gdk_seat_get_keyboard(seat));
        gboolean handled = FALSE;
        g_signal_emit_by_name(widget, release ? "key-release-event" : "key-press-event", event, &handled);
        CHECK(handled);
        gdk_event_free(event);
    }
    settle();
}

static void pointer(GdkEventType type, double x, double y, GdkModifierType state)
{
    GdkEvent *event = gdk_event_new(type);
    event->any.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget)));
    const char *signal;
    if (type == GDK_MOTION_NOTIFY) {
        event->motion.x = x; event->motion.y = y; event->motion.state = state;
        signal = "motion-notify-event";
    } else if (type == GDK_SCROLL) {
        event->scroll.x = x; event->scroll.y = y; event->scroll.state = state;
        event->scroll.direction = GDK_SCROLL_DOWN;
        signal = "scroll-event";
    } else {
        event->button.x = x; event->button.y = y; event->button.state = state;
        event->button.button = 1;
        signal = type == GDK_BUTTON_RELEASE ? "button-release-event" : "button-press-event";
    }
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(widget)));
    gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    gboolean handled = FALSE;
    g_signal_emit_by_name(widget, signal, event, &handled);
    CHECK(handled);
    gdk_event_free(event);
    settle();
}

static gboolean painted(GtkWidget *native, cairo_t *cr, gpointer data)
{
    (void)native; (void)cr;
    *(bool *)data = true;
    return FALSE;
}

static cairo_surface_t *capture(const char *name)
{
    GtkWidget *native = GTK_WIDGET(widget);
    /* Input completion does not finish GTK's queued size allocation. Wait
     * for a real draw before requesting an offscreen copy of the widget. */
    bool drawn = false;
    gulong handler = g_signal_connect_after(native, "draw", G_CALLBACK(painted), &drawn);
    guint timeout = g_timeout_add_seconds(60, deadline, "allocated widget draw");
    gtk_widget_queue_draw(native);
    while (!drawn) { frame(); if (!drawn) iterate(); }
    g_source_remove(timeout);
    g_signal_handler_disconnect(native, handler);
    int scale = gtk_widget_get_scale_factor(native);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        gtk_widget_get_allocated_width(native) * scale, gtk_widget_get_allocated_height(native) * scale);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    gtk_widget_draw(native, cr);
    CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    size_t length = strlen(output_directory) + strlen(name) + 2;
    char *path = malloc(length);
    CHECK(path != NULL);
    CHECK(snprintf(path, length, "%s/%s", output_directory, name) > 0);
    CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
    free(path);
    return surface;
}

static size_t differences(cairo_surface_t *left, cairo_surface_t *right)
{
    int width = cairo_image_surface_get_width(left), height = cairo_image_surface_get_height(left);
    CHECK(width == cairo_image_surface_get_width(right) && height == cairo_image_surface_get_height(right));
    const unsigned char *a = cairo_image_surface_get_data(left), *b = cairo_image_surface_get_data(right);
    int stride_a = cairo_image_surface_get_stride(left), stride_b = cairo_image_surface_get_stride(right);
    size_t count = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (memcmp(a + y * stride_a + x * 4, b + y * stride_b + x * 4, 4) != 0) count++;
        }
    }
    return count;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    output_directory = argv[2];
    setbuf(stdout, NULL);
    CHECK(setenv("GTK_IM_MODULE", "gtk-im-context-simple", 1) == 0);
    gtk_init(NULL, NULL);
    bus = ibus_bus_new();
    CHECK(ibus_bus_is_connected(bus));
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_gtk3_terminal_widget_config wc = boxrig_gtk3_terminal_widget_config_default();
    wc.render_policy = BOXRIG_GTK_RENDER_CAIRO;
    wc.ime_policy = BOXRIG_GTK_IME_IBUS;
    wc.candidate_policy = BOXRIG_UI_CANDIDATE_CLIENT;
    wc.view.blink.cursor_enabled = false;
    wc.view.blink.text_enabled = false;
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = &dispatch_requested;
    CHECK(boxrig_gtk3_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_set_mouse_router(widget, route_mouse, &mouse_routed) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_set_size_request(GTK_WIDGET(widget), 480, 320);
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(window);
    guint timeout = g_timeout_add_seconds(60, deadline, "mapped widget");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) || gtk_widget_get_allocated_width(GTK_WIDGET(widget)) < 2)
        iterate();
    g_source_remove(timeout);
    gtk_widget_grab_focus(GTK_WIDGET(widget));
    focused(true);
    await_client();
    /* The engine consumes this key without preedit, terminal text or candidate
     * updates, leaving a completed and stable baseline presentation. */
    key(GDK_KEY_Escape, 9);
    cairo_surface_t *before = capture("before.png");
    key(GDK_KEY_F13, 191);
    cairo_surface_t *shown = capture("shown.png");
    CHECK(differences(before, shown) > 128);
    boxrig_ui_view *view = boxrig_gtk3_terminal_widget_view(widget);
    boxrig_ui_candidate_view candidates;
    CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
    CHECK(candidates.snapshot.count == 5 && candidates.snapshot.cursor == 1);
    CHECK(strcmp(candidates.snapshot.items[1], "汽車") == 0);
    CHECK(strcmp(candidates.snapshot.labels[1], "s") == 0);
    CHECK(strcmp(candidates.snapshot.auxiliary, "説明🙂\n次の行") == 0);
    CHECK(candidates.layout.count == 3 && candidates.layout.revision == candidates.revision);
    boxrig_gtk_ime_state ime;
    CHECK(boxrig_gtk3_terminal_widget_get_ime_state(widget, &ime) == BOXRIG_UI_OK);
    CHECK(ime.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT && ime.candidate_reason == BOXRIG_UI_CANDIDATE_READY);
    CHECK(ime.candidate_count == 5 && ime.candidate_cursor == 1 && ime.candidate_visible && ime.auxiliary_visible);
    CHECK(boxrig_gtk3_terminal_widget_set_render_policy(widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk3_terminal_widget_set_render_policy(widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    const char mouse_mode[] = "\033[?1003h\033[?1006h";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)mouse_mode, sizeof(mouse_mode) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(mouse_mode) - 1);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    double inside_x = (candidates.layout.bounds.x + candidates.layout.bounds.width / 2.0) / scale;
    double inside_y = (candidates.layout.bounds.y + candidates.layout.bounds.height / 2.0) / scale;
    double outside_x = gtk_widget_get_allocated_width(GTK_WIDGET(widget)) - 40;
    double outside_y = gtk_widget_get_allocated_height(GTK_WIDGET(widget)) - 40;
    boxrig_ui_hit_test_result hit;
    CHECK(boxrig_ui_view_hit_test(view, (boxrig_ui_fixed)(inside_x * scale * BOXRIG_UI_FIXED_ONE),
        (boxrig_ui_fixed)(inside_y * scale * BOXRIG_UI_FIXED_ONE), &hit) == BOXRIG_UI_OK);
    CHECK(hit.overlay_roles == BOXRIG_UI_OVERLAY_CANDIDATES && !hit.inside_grid);
    pointer(GDK_BUTTON_PRESS, inside_x, inside_y, 0);
    pointer(GDK_MOTION_NOTIFY, outside_x, outside_y, GDK_BUTTON1_MASK);
    pointer(GDK_SCROLL, inside_x, inside_y, GDK_BUTTON1_MASK);
    key(GDK_KEY_F14, 192);
    pointer(GDK_BUTTON_RELEASE, outside_x, outside_y, GDK_BUTTON1_MASK);
    CHECK(mouse_routed == 0 && boxrig_terminal_output_peek(terminal, &bytes) == 0);
    /* Renderer changes can leave GTK allocation pending after input drains. */
    gtk_widget_queue_resize(GTK_WIDGET(widget));
    cairo_surface_t *hidden = capture("hidden.png");
    CHECK(differences(before, hidden) == 0);
    key(GDK_KEY_F15, 193);
    cairo_surface_t *reshown = capture("reshown.png");
    CHECK(differences(shown, reshown) == 0);
    pointer(GDK_BUTTON_PRESS, outside_x, outside_y, 0);
    pointer(GDK_BUTTON_RELEASE, inside_x, inside_y, GDK_BUTTON1_MASK);
    /* Terminal-owned releases bypass the application router, but still
     * balance the previously reported press when released above the panel. */
    CHECK(mouse_routed == 1);
    size_t mouse_bytes = boxrig_terminal_output_peek(terminal, &bytes);
    CHECK(mouse_bytes > 0 && memcmp(bytes, "\033[<", 3) == 0);
    CHECK(memchr(bytes, 'M', mouse_bytes) != NULL && memchr(bytes, 'm', mouse_bytes) != NULL);
    CHECK(boxrig_terminal_output_consume(terminal, mouse_bytes) == BOXRIG_STATUS_OK);
    key(GDK_KEY_F16, 194);
    CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
    CHECK(candidates.snapshot.cursor == 2 && candidates.layout.first == 0);
    key(GDK_KEY_F18, 196);
    CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
    CHECK(candidates.snapshot.cursor == 4 && candidates.layout.first == 3 && candidates.layout.count == 2);
    key(GDK_KEY_F13, 191);
    pointer(GDK_BUTTON_PRESS, inside_x, inside_y, 0);
    focused(false);
    settle();
    CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
    CHECK(candidates.snapshot.count == 0);
    cairo_surface_t *blurred = capture("blurred.png");
    cairo_surface_destroy(blurred);
    focused(true);
    await_client();
    /* A release can belong to another window after focus changes. A new
     * press starts a new sequence and cannot inherit that old capture. */
    pointer(GDK_BUTTON_PRESS, outside_x, outside_y, 0);
    pointer(GDK_MOTION_NOTIFY, outside_x - 10, outside_y, GDK_BUTTON1_MASK);
    pointer(GDK_BUTTON_RELEASE, outside_x - 10, outside_y, GDK_BUTTON1_MASK);
    CHECK(mouse_routed == 3);
    mouse_bytes = boxrig_terminal_output_peek(terminal, &bytes);
    CHECK(mouse_bytes > 0 && memchr(bytes, 'm', mouse_bytes) != NULL);
    CHECK(boxrig_terminal_output_consume(terminal, mouse_bytes) == BOXRIG_STATUS_OK);
    key(GDK_KEY_F13, 191);
    key(GDK_KEY_F24, 202);
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == strlen("汽車"));
    CHECK(memcmp(bytes, "汽車", strlen("汽車")) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen("汽車")) == BOXRIG_STATUS_OK);
    cairo_surface_t *committed = capture("committed.png");
    CHECK(differences(before, committed) == 0);
    cairo_surface_destroy(committed);
    cairo_surface_destroy(reshown);
    cairo_surface_destroy(hidden);
    cairo_surface_destroy(shown);
    cairo_surface_destroy(before);
    gtk_widget_destroy(window);
    g_object_unref(widget);
    g_object_unref(bus);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS GTK 3 candidate content, display, paging, pointer isolation, focus clearing and single commit");
    return 0;
}
