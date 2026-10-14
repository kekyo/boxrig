/* boxrig - Under MIT. */
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3.h"
#define Widget BoxrigGtk3TerminalWidget
#define widget_take_layout boxrig_gtk3_terminal_widget_take_layout_action
#define widget_complete_resize boxrig_gtk3_terminal_widget_complete_grid_resize
#define widget_visible boxrig_gtk3_terminal_widget_set_visible
#define widget_frame boxrig_gtk3_terminal_widget_frame
#define widget_composition boxrig_gtk3_terminal_widget_composition
#define widget_config boxrig_gtk3_terminal_widget_config
#define widget_config_default boxrig_gtk3_terminal_widget_config_default
#define widget_create boxrig_gtk3_terminal_widget_create
#define widget_ime_state boxrig_gtk3_terminal_widget_get_ime_state
#define widget_set_input_enabled boxrig_gtk3_terminal_widget_set_input_enabled
#define widget_diagnostic boxrig_gtk3_terminal_widget_get_diagnostic
#define widget_width gtk_widget_get_allocated_width
#define widget_height gtk_widget_get_allocated_height
#else
#include "boxrig/gtk4.h"
#define Widget BoxrigGtk4TerminalWidget
#define widget_take_layout boxrig_gtk4_terminal_widget_take_layout_action
#define widget_complete_resize boxrig_gtk4_terminal_widget_complete_grid_resize
#define widget_visible boxrig_gtk4_terminal_widget_set_visible
#define widget_frame boxrig_gtk4_terminal_widget_frame
#define widget_composition boxrig_gtk4_terminal_widget_composition
#define widget_config boxrig_gtk4_terminal_widget_config
#define widget_config_default boxrig_gtk4_terminal_widget_config_default
#define widget_create boxrig_gtk4_terminal_widget_create
#define widget_ime_state boxrig_gtk4_terminal_widget_get_ime_state
#define widget_set_input_enabled boxrig_gtk4_terminal_widget_set_input_enabled
#define widget_diagnostic boxrig_gtk4_terminal_widget_get_diagnostic
#define widget_width gtk_widget_get_width
#define widget_height gtk_widget_get_height
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL GTK fcitx %d: %s\n", __LINE__, #value); abort(); } } while (0)
static Widget *widget;
static boxrig_terminal *terminal;
static bool dispatch_requested;
#ifndef BOXRIG_TEST_GTK3
static GtkEventController *key_controller, *focus_controller;
GtkEventController *__real_gtk_event_controller_key_new(void);
GtkEventController *__wrap_gtk_event_controller_key_new(void)
{ return key_controller = __real_gtk_event_controller_key_new(); }
GtkEventController *__real_gtk_event_controller_focus_new(void);
GtkEventController *__wrap_gtk_event_controller_focus_new(void)
{ return focus_controller = __real_gtk_event_controller_focus_new(); }
#endif
static void request_frame(void *data)
{ (void)data; dispatch_requested = true; g_main_context_wakeup(NULL); }
static gboolean deadline(gpointer data)
{ fprintf(stderr, "FAIL GTK fcitx: %s\n", (const char *)data); abort(); }
static void iterate(void)
{
    gboolean block = !dispatch_requested;
    dispatch_requested = false;
    g_main_context_iteration(NULL, block);
}
static void frame(void)
{
    dispatch_requested = false;
    boxrig_ui_component_frame_result result;
    CHECK(widget_frame(widget, &result) == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (widget_take_layout(widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(widget_complete_resize(widget, action.revision, true) == BOXRIG_UI_OK);
        result.additional_dispatch = true;
    }
    dispatch_requested |= result.additional_dispatch;
}
static void focus(bool active)
{
#ifdef BOXRIG_TEST_GTK3
    GdkEvent *event = gdk_event_new(GDK_FOCUS_CHANGE);
    event->focus_change.in = active;
    event->focus_change.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget)));
    gboolean handled;
    g_signal_emit_by_name(widget, active ? "focus-in-event" : "focus-out-event", event, &handled);
    gdk_event_free(event);
#else
    g_signal_emit_by_name(focus_controller, active ? "enter" : "leave");
#endif
}
static void settle(void)
{
    guint timer = g_timeout_add_seconds(60, deadline, "input completion");
    for (;;) {
        frame();
        boxrig_gtk_ime_state ime;
        boxrig_ui_component_diagnostic state;
        CHECK(widget_ime_state(widget, &ime) == BOXRIG_UI_OK);
        CHECK(widget_diagnostic(widget, &state) == BOXRIG_UI_OK);
        if (ime.ibus_pending == 0 && ime.ibus_contexts == 1 && state.native_input_queue_depth == 0) break;
        iterate();
    }
    g_source_remove(timer);
}
static void key(guint symbol)
{
#ifdef BOXRIG_TEST_GTK3
    for (int release = 0; release < 2; release++) {
        GdkEvent *event = gdk_event_new(release ? GDK_KEY_RELEASE : GDK_KEY_PRESS);
        event->key.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget)));
        event->key.time = 1234;
        event->key.keyval = symbol;
        event->key.hardware_keycode = 38;
        GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(widget)));
        CHECK(seat != NULL && gdk_seat_get_keyboard(seat) != NULL);
        gdk_event_set_device(event, gdk_seat_get_keyboard(seat));
        gboolean handled;
        g_signal_emit_by_name(widget, release ? "key-release-event" : "key-press-event", event, &handled);
        CHECK(handled);
        gdk_event_free(event);
    }
#else
    gboolean handled;
    g_signal_emit_by_name(key_controller, "key-pressed", symbol, 38, 0, &handled);
    CHECK(handled);
    g_signal_emit_by_name(key_controller, "key-released", symbol, 38, 0);
#endif
    settle();
}
static boxrig_composition_view composition(void)
{
    boxrig_composition_view result;
    CHECK(boxrig_composition_get_view(widget_composition(widget), &result) == BOXRIG_COMPOSITION_OK);
    return result;
}
static void output(const char *expected)
{
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == strlen(expected));
    CHECK(memcmp(bytes, expected, strlen(expected)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen(expected)) == BOXRIG_STATUS_OK);
}
int main(void)
{
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    widget_config wc = widget_config_default();
    wc.ime_policy = BOXRIG_GTK_IME_FCITX5;
    wc.candidate_policy = BOXRIG_UI_CANDIDATE_SYSTEM;
    wc.render_policy = BOXRIG_GTK_RENDER_CAIRO;
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = &dispatch_requested;
    CHECK(widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(widget_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_set_size_request(GTK_WIDGET(widget), 480, 240);
#ifdef BOXRIG_TEST_GTK3
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_show_all(window);
#else
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(widget));
#endif
    gtk_window_present(GTK_WINDOW(window));
    guint timer = g_timeout_add_seconds(60, deadline, "window mapping");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) || widget_width(GTK_WIDGET(widget)) <= 1) iterate();
    g_source_remove(timer);
    frame();
    boxrig_gtk_ime_state state;
    CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
    if (GTK_MAJOR_VERSION == 4 && strcmp(state.display_type, "GdkX11Display") == 0) {
        CHECK(state.active_route == BOXRIG_GTK_IME_ROUTE_GTK);
        CHECK(strcmp(state.gtk_module, "fcitx") == 0);
        CHECK(state.ibus_contexts == 0);
        puts("PASS GTK 4 X11 delegates explicit fcitx5 to the GTK module");
        goto cleanup;
    }
    focus(true);
    settle();
    CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.active_route == BOXRIG_GTK_IME_ROUTE_FCITX5);
    CHECK(state.reason == BOXRIG_GTK_IME_REASON_EXPLICIT_FCITX5);
    CHECK(state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_SYSTEM);
    key('a');
    boxrig_composition_view text = composition();
    CHECK(text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
    CHECK(text.caret_offset == 3);
    key(GDK_KEY_Escape);
    CHECK(composition().text_length == 0);
    key('a');
    key('b');
    output("汽車🙂");
    key('u');
    output("u");
    key('a');
    focus(false);
    timer = g_timeout_add_seconds(60, deadline, "focus commit");
    const uint8_t *bytes;
    do { frame(); if (boxrig_terminal_output_peek(terminal, &bytes) == 0) iterate(); }
    while (boxrig_terminal_output_peek(terminal, &bytes) == 0);
    g_source_remove(timer);
    output("き");
    printf("PASS GTK %u direct fcitx5 selection, preedit, commit, fallback and focus\n", gtk_get_major_version());
cleanup:
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return 0;
}
