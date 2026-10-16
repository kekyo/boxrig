/* boxrig - Under MIT. */
#include "boxrig/gtk4.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL GTK 4 candidates %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

static BoxrigGtk4TerminalWidget *widget;
static boxrig_terminal *terminal;
static IBusBus *bus;
static bool dispatch_requested;
static const char *output_directory;
static GtkEventController *key_controller, *focus_controller;
static GtkGesture *click_gesture;
static GtkEventController *motion_controller, *scroll_controller;
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
    fprintf(stderr, "FAIL GTK 4 candidates: no completion for %s\n", (char *)data);
    abort();
}

static void frame(void)
{
    boxrig_ui_component_frame_result result;
    dispatch_requested = false;
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &result) == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (boxrig_gtk4_terminal_widget_take_layout_action(widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(widget, action.revision, true) == BOXRIG_UI_OK);
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

GtkEventController *__real_gtk_event_controller_key_new(void);
GtkEventController *__wrap_gtk_event_controller_key_new(void)
{
    key_controller = __real_gtk_event_controller_key_new();
    return key_controller;
}
GtkEventController *__real_gtk_event_controller_focus_new(void);
GtkEventController *__wrap_gtk_event_controller_focus_new(void)
{
    focus_controller = __real_gtk_event_controller_focus_new();
    return focus_controller;
}
GtkGesture *__real_gtk_gesture_click_new(void);
GtkGesture *__wrap_gtk_gesture_click_new(void)
{
    click_gesture = __real_gtk_gesture_click_new();
    return click_gesture;
}
GtkEventController *__real_gtk_event_controller_motion_new(void);
GtkEventController *__wrap_gtk_event_controller_motion_new(void)
{
    motion_controller = __real_gtk_event_controller_motion_new();
    return motion_controller;
}
GtkEventController *__real_gtk_event_controller_scroll_new(GtkEventControllerScrollFlags flags);
GtkEventController *__wrap_gtk_event_controller_scroll_new(GtkEventControllerScrollFlags flags)
{
    scroll_controller = __real_gtk_event_controller_scroll_new(flags);
    return scroll_controller;
}

static void focused(bool active)
{
    g_signal_emit_by_name(focus_controller, active ? "enter" : "leave");
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
        "org.boxrig.IMEProbe", "WaitForClient", g_variant_new("(s)", "boxrig-gtk4-wayland"),
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
        CHECK(boxrig_gtk4_terminal_widget_get_ime_state(widget, &ime) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &state) == BOXRIG_UI_OK);
        if (ime.direct.pending == 0 && state.native_input_queue_depth == 0 &&
            boxrig_gtk4_terminal_widget_has_presentation(widget)) break;
        iterate();
    }
    g_source_remove(timeout);
}

static void key(guint symbol, guint code)
{
    gboolean handled = FALSE;
    g_signal_emit_by_name(key_controller, "key-pressed", symbol, code, 0, &handled);
    CHECK(handled);
    g_signal_emit_by_name(key_controller, "key-released", symbol, code, 0);
    settle();
}

static void pointer(const char *action, double x, double y)
{
    if (strcmp(action, "scroll") == 0) {
        g_signal_emit_by_name(motion_controller, "enter", x, y);
        gboolean handled = FALSE;
        g_signal_emit_by_name(scroll_controller, "scroll", 0.0, 1.0, &handled);
        CHECK(handled);
    } else if (strcmp(action, "motion") == 0) {
        g_signal_emit_by_name(motion_controller, action, x, y);
    } else {
        g_signal_emit_by_name(click_gesture, action, 1, x, y);
    }
    settle();
}

static cairo_surface_t *capture(const char *name)
{
    GtkWidget *native = GTK_WIDGET(widget);
    int scale = gtk_widget_get_scale_factor(native);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        gtk_widget_get_width(native) * scale, gtk_widget_get_height(native) * scale);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gtk_widget_snapshot_child(gtk_widget_get_parent(native), native, snapshot);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    gsk_render_node_draw(node, cr);
    gsk_render_node_unref(node);
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
    gtk_init();
    bus = ibus_bus_new();
    CHECK(ibus_bus_is_connected(bus));
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_gtk4_terminal_widget_config wc = boxrig_gtk4_terminal_widget_config_default();
    wc.render_policy = BOXRIG_GTK_RENDER_CAIRO;
    wc.ime_policy = BOXRIG_GTK_IME_IBUS;
    wc.view.blink.cursor_enabled = false;
    wc.view.blink.text_enabled = false;
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = &dispatch_requested;
    CHECK(boxrig_gtk4_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk4_terminal_widget_set_mouse_router(widget, route_mouse, &mouse_routed) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(widget));
    gtk_widget_set_size_request(GTK_WIDGET(widget), 480, 320);
    CHECK(boxrig_gtk4_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
    guint timeout = g_timeout_add_seconds(60, deadline, "mapped widget");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) || gtk_widget_get_width(GTK_WIDGET(widget)) < 2)
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
    boxrig_ui_view *view = boxrig_gtk4_terminal_widget_view(widget);
    boxrig_ui_candidate_view candidates;
    CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
    CHECK(candidates.snapshot.count == 5 && candidates.snapshot.cursor == 1);
    CHECK(strcmp(candidates.snapshot.items[1], "汽車") == 0);
    CHECK(strcmp(candidates.snapshot.labels[1], "s") == 0);
    CHECK(strcmp(candidates.snapshot.auxiliary, "説明🙂\n次の行") == 0);
    CHECK(candidates.layout.count == 3 && candidates.layout.revision == candidates.revision);
    boxrig_gtk_ime_state ime;
    CHECK(boxrig_gtk4_terminal_widget_get_ime_state(widget, &ime) == BOXRIG_UI_OK);
    CHECK(ime.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT && ime.candidate_reason == BOXRIG_UI_CANDIDATE_READY);
    CHECK(ime.candidate_count == 5 && ime.candidate_cursor == 1 && ime.candidate_visible && ime.auxiliary_visible);
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    const char mouse_mode[] = "\033[?1003h\033[?1006h";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)mouse_mode, sizeof(mouse_mode) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(mouse_mode) - 1);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    double inside_x = (candidates.layout.bounds.x + candidates.layout.bounds.width / 2.0) / scale;
    double inside_y = (candidates.layout.bounds.y + candidates.layout.bounds.height / 2.0) / scale;
    double outside_x = gtk_widget_get_width(GTK_WIDGET(widget)) - 40;
    double outside_y = gtk_widget_get_height(GTK_WIDGET(widget)) - 40;
    pointer("pressed", inside_x, inside_y);
    pointer("motion", outside_x, outside_y);
    pointer("scroll", inside_x, inside_y);
    key(GDK_KEY_F14, 192);
    pointer("released", outside_x, outside_y);
    CHECK(mouse_routed == 0 && boxrig_terminal_output_peek(terminal, &bytes) == 0);
    cairo_surface_t *hidden = capture("hidden.png");
    CHECK(differences(before, hidden) == 0);
    key(GDK_KEY_F15, 193);
    cairo_surface_t *reshown = capture("reshown.png");
    CHECK(differences(shown, reshown) == 0);
    pointer("pressed", outside_x, outside_y);
    pointer("released", inside_x, inside_y);
    /* A terminal-owned press must receive its release above the panel. */
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
    pointer("pressed", inside_x, inside_y);
    focused(false);
    settle();
    CHECK(boxrig_ui_view_get_candidates(view, &candidates) == BOXRIG_UI_OK);
    CHECK(candidates.snapshot.count == 0);
    cairo_surface_t *blurred = capture("blurred.png");
    cairo_surface_destroy(blurred);
    focused(true);
    await_client();
    /* The old release can belong to another window; this press is new. */
    pointer("pressed", outside_x, outside_y);
    pointer("motion", outside_x - 10, outside_y);
    pointer("released", outside_x - 10, outside_y);
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
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(widget);
    g_object_unref(bus);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS GTK 4 candidate content, display, paging, pointer isolation, focus clearing and single commit");
    return 0;
}
