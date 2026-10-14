/* boxrig - Under MIT. */
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3.h"
#define Widget BoxrigGtk3TerminalWidget
#define widget_take_layout boxrig_gtk3_terminal_widget_take_layout_action
#define widget_complete_resize boxrig_gtk3_terminal_widget_complete_grid_resize
#define widget_visible boxrig_gtk3_terminal_widget_set_visible
#define widget_frame boxrig_gtk3_terminal_widget_frame
#define widget_composition boxrig_gtk3_terminal_widget_composition
#define widget_view boxrig_gtk3_terminal_widget_view
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
#define widget_view boxrig_gtk4_terminal_widget_view
#define widget_config boxrig_gtk4_terminal_widget_config
#define widget_config_default boxrig_gtk4_terminal_widget_config_default
#define widget_create boxrig_gtk4_terminal_widget_create
#define widget_ime_state boxrig_gtk4_terminal_widget_get_ime_state
#define widget_set_input_enabled boxrig_gtk4_terminal_widget_set_input_enabled
#define widget_diagnostic boxrig_gtk4_terminal_widget_get_diagnostic
#define widget_width gtk_widget_get_width
#define widget_height gtk_widget_get_height
#endif
#include "boxrig/ui/editable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL GTK fcitx4 %d: %s\n", __LINE__, #value); abort(); } } while (0)
static Widget *widget;
static boxrig_terminal *terminal;
static bool dispatch_requested;
static size_t edits;
static bool focus_edit;
static bool panels;
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
    boxrig_ui_status status = widget_frame(widget, &result);
    if (status != BOXRIG_UI_OK) fprintf(stderr, "fcitx4 frame status=%d, size=%dx%d\n", (int)status,
        widget_width(GTK_WIDGET(widget)), widget_height(GTK_WIDGET(widget)));
    CHECK(status == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (widget_take_layout(widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, action.value.grid_resize.rows, action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(widget_complete_resize(widget, action.revision, true) == BOXRIG_UI_OK);
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
        CHECK(widget_ime_state(widget, &ime) == BOXRIG_UI_OK);
        CHECK(widget_diagnostic(widget, &state) == BOXRIG_UI_OK);
        if (ime.direct.pending == 0 && ime.direct.contexts == contexts && state.native_input_queue_depth == 0) break;
        iterate();
    }
    g_source_remove(timer);
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
    settle(active ? 1 : 0);
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
    settle(1);
}
static boxrig_composition_view composition(void)
{
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(widget_composition(widget), &view) == BOXRIG_COMPOSITION_OK);
    return view;
}
static void output(const char *expected)
{
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == strlen(expected));
    CHECK(expected[0] == 0 || memcmp(bytes, expected, strlen(expected)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen(expected)) == BOXRIG_STATUS_OK);
}
static boxrig_ui_candidate_view panel_state(void)
{
    boxrig_ui_candidate_view view;
    CHECK(boxrig_ui_view_get_candidates(widget_view(widget), &view) == BOXRIG_UI_OK);
    CHECK(view.snapshot.count == 0 && !composition().active);
    return view;
}
static void mock_panel(void)
{
    key('a');
    boxrig_ui_candidate_view view = panel_state();
    CHECK(view.snapshot.format == BOXRIG_UI_CANDIDATE_STRING_PANEL && view.snapshot.visible);
    CHECK(view.snapshot.panel_preedit != NULL);
    CHECK(strcmp(view.snapshot.panel_preedit, "きしゃ🙂") == 0 && view.snapshot.panel_cursor == 6);
    CHECK(view.layout.panel_preedit.height > 0 && view.layout.panel_caret.height > 0);
    boxrig_gtk_ime_state state;
    CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT && state.candidate_reason == BOXRIG_UI_CANDIDATE_READY);
    key('p');
    view = panel_state();
    CHECK(strcmp(view.snapshot.panel_text, "1. 汽車 2号 2. 記者 🙂") == 0);
    CHECK(view.layout.panel_text.height > 0 && view.layout.auxiliary.height > 0);
    CHECK(view.layout.bounds.x + view.layout.bounds.width <= widget_width(GTK_WIDGET(widget)));
    CHECK(view.layout.bounds.y + view.layout.bounds.height <= widget_height(GTK_WIDGET(widget)));
    key('n');
    CHECK(strcmp(panel_state().snapshot.panel_text, "1. 次ページ 2. 候補") == 0);
    output("");
    key('b');
    CHECK(!panel_state().snapshot.visible);
    output("汽車🙂");
    key('a');
    key(GDK_KEY_Escape);
    CHECK(!panel_state().snapshot.visible);
    output("");
    key('j');
    CHECK(panel_state().snapshot.auxiliary_above_visible);
    key('a');
    CHECK(widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
    settle(0);
    CHECK(panel_state().snapshot.panel_text == NULL);
    output("");
    CHECK(widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
    settle(1);
    key('p');
    focus(false);
    CHECK(panel_state().snapshot.panel_text == NULL);
    output("server-commit");
    puts("PASS GTK fcitx4 string panel, keyboard pages, caret, cancellation, focus commit and input disabling");
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
static boxrig_ui_status real_edit(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    const char *converted = *(char **)data;
    CHECK(converted != NULL);
    CHECK(request->range.offset == 3 && request->range.length == 6);
    CHECK(request->replacement_length == strlen(converted));
    CHECK(memcmp(request->replacement, converted, strlen(converted)) == 0);
    CHECK(proposed->context_length == strlen(converted) + 6);
    CHECK(memcmp(proposed->context, "前", 3) == 0);
    CHECK(memcmp(proposed->context + 3, converted, strlen(converted)) == 0);
    CHECK(memcmp(proposed->context + 3 + strlen(converted), "後", 3) == 0);
    edits++;
    return BOXRIG_UI_OK;
}
static void real_mozc(void)
{
    const char *display = g_getenv("DISPLAY");
    CHECK(display != NULL && strrchr(display, ':') != NULL);
    char *service = g_strdup_printf("org.fcitx.Fcitx-%d", atoi(strrchr(display, ':') + 1));
    const char *address = g_getenv("BOXRIG_TEST_FCITX4_REAL_ADDRESS");
    CHECK(address != NULL);
    GDBusConnection *bus = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
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
    if (panels) {
        boxrig_ui_candidate_view view = panel_state();
        CHECK(view.snapshot.format == BOXRIG_UI_CANDIDATE_STRING_PANEL && view.snapshot.visible);
        CHECK(view.snapshot.panel_preedit != NULL && g_str_has_prefix(view.snapshot.panel_preedit, "きしゃ"));
        CHECK(view.snapshot.panel_cursor == strlen("きしゃ"));
        CHECK(view.layout.panel_preedit.height > 0 && view.layout.panel_caret.height > 0);
        boxrig_gtk_ime_state state;
        CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
        CHECK(state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_CLIENT);
    } else {
        CHECK(text.active && text.text_length == strlen("きしゃ") && memcmp(text.text, "きしゃ", text.text_length) == 0);
        CHECK(text.segment_count != 0 && (text.segments[0].feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) != 0);
    }
    output("");
    key(GDK_KEY_space);
    char *converted;
    if (panels) {
        key(GDK_KEY_space);
        boxrig_ui_candidate_view view = panel_state();
        CHECK(view.snapshot.panel_text != NULL && view.snapshot.panel_text[0] != 0);
        CHECK(view.layout.panel_text.height > 0);
        char *first = g_strdup(view.snapshot.panel_text);
        key(GDK_KEY_Page_Down);
        view = panel_state();
        CHECK(view.snapshot.panel_text != NULL && view.snapshot.panel_text[0] != 0);
        CHECK(strcmp(view.snapshot.panel_text, first) != 0);
        g_free(first);
        /* The panel preedit can include Mozc's usage/footer text. F7 gives a
         * deterministic conversion without treating the panel as a commit. */
        key(GDK_KEY_F7);
        view = panel_state();
        CHECK(view.snapshot.panel_preedit != NULL && g_str_has_prefix(view.snapshot.panel_preedit, "キシャ"));
        converted = g_strdup("キシャ");
    } else {
        text = composition();
        CHECK(text.active && text.text_length != 0);
        CHECK(text.text_length != strlen("きしゃ") || memcmp(text.text, "きしゃ", text.text_length) != 0);
        converted = g_strndup(text.text, text.text_length);
    }
    key(GDK_KEY_Return);
    CHECK(!composition().active);
    if (panels) CHECK(!panel_state().snapshot.visible);
    output(converted);
    g_free(converted);
    for (const char *letter = "kisha"; *letter != 0; letter++) key((guint)*letter);
    key(GDK_KEY_Escape);
    CHECK(!composition().active);
    if (panels) CHECK(!panel_state().snapshot.visible);
    output("");
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(widget_view(widget));
    boxrig_ui_editable_context_input context = {.document_identity = 1, .document_revision = 1,
        .context = "前かな後", .context_length = strlen("前かな後"), .cursor = 9, .anchor = 3};
    converted = NULL;
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, real_edit, &converted) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    for (const char *letter = "kisha"; *letter != 0; letter++) key((guint)*letter);
    key(GDK_KEY_space);
    if (panels) {
        key(GDK_KEY_F7);
        boxrig_ui_candidate_view view = panel_state();
        CHECK(view.snapshot.panel_preedit != NULL && g_str_has_prefix(view.snapshot.panel_preedit, "キシャ"));
        converted = g_strdup("キシャ");
    } else {
        text = composition();
        CHECK(text.active && text.text_length != 0);
        converted = g_strndup(text.text, text.text_length);
    }
    key(GDK_KEY_Return);
    CHECK(edits == 1 && !composition().active);
    output("");
    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, NULL, NULL) == BOXRIG_UI_OK);
    g_free(converted);
    key('a');
    focus(false);
    output(g_getenv("BOXRIG_TEST_FCITX4_NO_FOCUS_COMMIT") == NULL ? "あ" : "");
    g_free(service);
    CHECK(g_dbus_connection_close_sync(bus, NULL, NULL));
    g_object_unref(bus);
    puts(panels ? "PASS GTK real fcitx4/Mozc string panel, keyboard paging, document edit, cancellation and focus commit" :
        "PASS GTK real fcitx4/Mozc decorated preedit, conversion, document edit, cancellation and configured focus commit");
}
int main(void)
{
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    panels = g_getenv("BOXRIG_TEST_FCITX4_PANELS") != NULL;
    bool automatic = g_getenv("BOXRIG_TEST_IME_AUTO") != NULL;
    if (automatic) g_object_set(gtk_settings_get_default(), "gtk-im-module", "gtk-im-context-simple", NULL);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_model *model;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    widget_config wc = widget_config_default();
    wc.ime_policy = automatic ? BOXRIG_GTK_IME_AUTO : BOXRIG_GTK_IME_FCITX4;
    wc.candidate_policy = panels ? BOXRIG_UI_CANDIDATE_CLIENT : BOXRIG_UI_CANDIDATE_SYSTEM;
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
    gtk_widget_set_visible(window, true);
#endif
    gtk_window_present(GTK_WINDOW(window));
    guint timer = g_timeout_add_seconds(60, deadline, "window mapping");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) || widget_width(GTK_WIDGET(widget)) <= 1) iterate();
    g_source_remove(timer);
    focus(true);
    boxrig_gtk_ime_state state;
    CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
    CHECK(state.active_route == BOXRIG_GTK_IME_ROUTE_FCITX4);
    CHECK(state.direct.backend == BOXRIG_IME_BACKEND_FCITX4);
    CHECK(automatic ? state.direct.selection_reason == BOXRIG_IME_SELECTION_SINGLE :
        state.reason == BOXRIG_GTK_IME_REASON_EXPLICIT_FCITX4);
    CHECK(state.candidate_owner == (panels ? BOXRIG_UI_CANDIDATE_OWNER_NONE : BOXRIG_UI_CANDIDATE_OWNER_SYSTEM));
    CHECK(state.candidate_valid);
    if (g_getenv("BOXRIG_TEST_FCITX_REAL") != NULL) { real_mozc(); goto cleanup; }
    if (panels) { mock_panel(); goto cleanup; }
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
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(widget_view(widget));
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
