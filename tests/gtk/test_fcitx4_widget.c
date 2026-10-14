/* boxrig - Under MIT. */
#include "boxrig/gtk3.h"
#include "boxrig/ui/editable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL GTK fcitx4 %d: %s\n", __LINE__, #value); abort(); } } while (0)
static BoxrigGtk3TerminalWidget *widget;
static boxrig_terminal *terminal;
static bool dispatch_requested;
static size_t edits;
static bool focus_edit;
static void request_frame(void *data)
{ (void)data; dispatch_requested = true; g_main_context_wakeup(NULL); }
static gboolean deadline(gpointer data)
{ fprintf(stderr, "FAIL GTK fcitx4: %s\n", (const char *)data); abort(); }
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
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &result) == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (boxrig_gtk3_terminal_widget_take_layout_action(widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(widget, action.revision, true) == BOXRIG_UI_OK);
        result.additional_dispatch = true;
    }
    dispatch_requested |= result.additional_dispatch;
}
static void settle(size_t contexts)
{
    guint timer = g_timeout_add_seconds(60, deadline, "input completion");
    for (;;) {
        frame();
        boxrig_gtk_ime_state ime;
        boxrig_ui_component_diagnostic state;
        CHECK(boxrig_gtk3_terminal_widget_get_ime_state(widget, &ime) == BOXRIG_UI_OK);
        CHECK(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &state) == BOXRIG_UI_OK);
        if (ime.direct.pending == 0 && ime.direct.contexts == contexts && state.native_input_queue_depth == 0) break;
        iterate();
    }
    g_source_remove(timer);
}
static void focus(bool active)
{
    GdkEvent *event = gdk_event_new(GDK_FOCUS_CHANGE);
    event->focus_change.in = active;
    event->focus_change.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(widget)));
    gboolean handled;
    g_signal_emit_by_name(widget, active ? "focus-in-event" : "focus-out-event", event, &handled);
    gdk_event_free(event);
    settle(active ? 1 : 0);
}
static void key(guint symbol)
{
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
    settle(1);
}
static boxrig_composition_view composition(void)
{
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(boxrig_gtk3_terminal_widget_composition(widget), &view) == BOXRIG_COMPOSITION_OK);
    return view;
}
static void output(const char *expected)
{
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == strlen(expected));
    CHECK(expected[0] == 0 || memcmp(bytes, expected, strlen(expected)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen(expected)) == BOXRIG_STATUS_OK);
}
static boxrig_ui_status edit(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    (void)data;
    const char *replacement = focus_edit ? "server-commit" : "記者";
    const char *context = focus_edit ? "前かなserver-commit後" : "前記者後";
    CHECK(request->range.offset == (focus_edit ? 9u : 3u) && request->range.length == (focus_edit ? 0u : 6u));
    CHECK(request->replacement_length == strlen(replacement) && memcmp(request->replacement, replacement, strlen(replacement)) == 0);
    CHECK(proposed->context_length == strlen(context) && memcmp(proposed->context, context, strlen(context)) == 0);
    edits++;
    return BOXRIG_UI_OK;
}
static void real_mozc(void)
{
    const char *display = g_getenv("DISPLAY");
    CHECK(display != NULL && strrchr(display, ':') != NULL);
    char *service = g_strdup_printf("org.fcitx.Fcitx-%d", atoi(strrchr(display, ':') + 1));
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(bus != NULL);
    GVariant *reply = g_dbus_connection_call_sync(bus, service, "/inputmethod", "org.fcitx.Fcitx.InputMethod",
        "SetCurrentIM", g_variant_new("(s)", "mozc"), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    g_variant_unref(reply);
    reply = g_dbus_connection_call_sync(bus, service, "/inputmethod", "org.fcitx.Fcitx.InputMethod",
        "ActivateIM", NULL, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    g_variant_unref(reply);
    for (const char *letter = "kisha"; *letter != 0; letter++) key((guint)*letter);
    boxrig_composition_view text = composition();
    CHECK(text.active && text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
    output("");
    key(GDK_KEY_space);
    text = composition();
    CHECK(text.active && text.text_length != 0);
    CHECK(text.text_length != strlen("きしゃ") || memcmp(text.text, "きしゃ", text.text_length) != 0);
    char *converted = g_strndup(text.text, text.text_length);
    key(GDK_KEY_Return);
    CHECK(!composition().active);
    output(converted);
    g_free(converted);
    for (const char *letter = "kisha"; *letter != 0; letter++) key((guint)*letter);
    key(GDK_KEY_Escape);
    CHECK(!composition().active);
    output("");
    key('a');
    focus(false);
    output(g_getenv("BOXRIG_TEST_FCITX4_NO_FOCUS_COMMIT") == NULL ? "あ" : "");
    g_free(service);
    g_object_unref(bus);
    puts("PASS GTK 3 real fcitx4/Mozc conversion, commit, cancellation and configured focus commit");
}
int main(void)
{
    gtk_init(NULL, NULL);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_gtk3_terminal_widget_config wc = boxrig_gtk3_terminal_widget_config_default();
    wc.ime_policy = BOXRIG_GTK_IME_FCITX4;
    wc.candidate_policy = BOXRIG_UI_CANDIDATE_SYSTEM;
    wc.render_policy = BOXRIG_GTK_RENDER_CAIRO;
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = &dispatch_requested;
    CHECK(boxrig_gtk3_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    CHECK(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_set_size_request(GTK_WIDGET(widget), 480, 240);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
    guint timer = g_timeout_add_seconds(60, deadline, "window mapping");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget))) iterate();
    g_source_remove(timer);
    focus(true);
    boxrig_gtk_ime_state state;
    CHECK(boxrig_gtk3_terminal_widget_get_ime_state(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.active_route == BOXRIG_GTK_IME_ROUTE_FCITX4 && state.reason == BOXRIG_GTK_IME_REASON_EXPLICIT_FCITX4);
    CHECK(state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_SYSTEM && state.candidate_valid && !state.candidate_surface_relative);
    if (g_getenv("BOXRIG_TEST_FCITX_REAL") != NULL) { real_mozc(); goto cleanup; }
    key('a');
    boxrig_composition_view text = composition();
    CHECK(text.active && text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
    CHECK(text.segment_count == 2 && text.caret_offset == 3);
    CHECK(text.segments[0].feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);
    CHECK((text.segments[1].feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) == 0);
    key(GDK_KEY_Escape);
    CHECK(!composition().active);
    key('a');
    key('b');
    output("汽車🙂");
    key('u');
    output("u");
    key('a');
    focus(false);
    output("server-commit");
    focus(true);
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(boxrig_gtk3_terminal_widget_view(widget));
    boxrig_ui_editable_context_input context = {.document_identity = 1, .document_revision = 1,
        .context = "前かな後", .context_length = strlen("前かな後"), .cursor = 9, .anchor = 9};
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, edit, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    key('d');
    CHECK(edits == 1);
    context.document_revision = 3;
    CHECK(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    focus_edit = true;
    key('a');
    focus(false);
    CHECK(edits == 2);
    output("");
    puts("PASS GTK 3 fcitx4 system preedit, input, cancellation and focus commit to the original document");
cleanup:
    gtk_widget_destroy(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return 0;
}
