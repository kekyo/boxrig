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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boxrig/ui/editable.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL GTK IBus %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

#ifdef BOXRIG_TEST_IME_X11
#if GTK_MAJOR_VERSION == 3
#include <gdk/gdkx.h>
#else
#include <gdk/x11/gdkx.h>
#endif
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include "../x11_video.h"
static Display *xdisplay;
static Window xwindow;
#endif
static bool direct_ibus;

static GtkWidget *input_widget;
static bool dispatch_requested;
static boxrig_terminal *frame_terminal;
static void request_frame(void *data)
{
    (void)data;
    dispatch_requested = true;
    g_main_context_wakeup(NULL);
}
static void iterate(void)
{
    gboolean block = !dispatch_requested;
    dispatch_requested = false;
    g_main_context_iteration(NULL, block);
}
#ifndef BOXRIG_TEST_GTK3
static GtkEventController *key_controller, *focus_controller;
static GtkIMContext *observed_im_context;
static unsigned observed_f10, observed_f10_release;
static gboolean observe_native_key(GtkEventControllerKey *controller, guint symbol,
    guint code, GdkModifierType modifiers, gpointer data)
{
    (void)controller; (void)symbol; (void)modifiers; (void)data;
    if (code == 76) observed_f10++;
    return FALSE;
}
static void observe_native_release(GtkEventControllerKey *controller, guint symbol,
    guint code, GdkModifierType modifiers, gpointer data)
{
    (void)controller; (void)symbol; (void)modifiers; (void)data;
    if (code == 76) observed_f10_release++;
}
GtkIMContext *__real_gtk_im_multicontext_new(void);
GtkIMContext *__wrap_gtk_im_multicontext_new(void)
{
    observed_im_context = __real_gtk_im_multicontext_new();
    return observed_im_context;
}
GtkEventController *__real_gtk_event_controller_key_new(void);
GtkEventController *__wrap_gtk_event_controller_key_new(void)
{
    key_controller = __real_gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(observe_native_key), NULL);
    g_signal_connect(key_controller, "key-released", G_CALLBACK(observe_native_release), NULL);
    return key_controller;
}
GtkEventController *__real_gtk_event_controller_focus_new(void);
GtkEventController *__wrap_gtk_event_controller_focus_new(void)
{
    focus_controller = __real_gtk_event_controller_focus_new();
    return focus_controller;
}

#endif

static gboolean deadline(gpointer data)
{
    fprintf(stderr, "FAIL GTK IBus: no completion for %s\n", (const char *)data);
    abort();
}

static void frame(Widget *widget)
{
    boxrig_ui_component_frame_result result;
    dispatch_requested = false;
    CHECK(widget_frame(widget, &result) == BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (widget_take_layout(widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(frame_terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(widget_complete_resize(widget, action.revision, true) == BOXRIG_UI_OK);
        result.additional_dispatch = true;
    }
    /* Keep GTK's event and drawing dispatch between frame requests. Draining
     * only boxrig frames can starve the very callback that makes one ready. */
    dispatch_requested |= result.additional_dispatch;
}

static boxrig_composition_view composition(Widget *widget)
{
    boxrig_composition_view view;
    CHECK(boxrig_composition_get_view(widget_composition(widget), &view) ==
        BOXRIG_COMPOSITION_OK);
    return view;
}

static void focus(bool active)
{
#ifdef BOXRIG_TEST_IME_X11
    /* Focusing the root can retain pointer focus when a scaled window covers
     * the pointer. None unambiguously removes keyboard focus. */
    CHECK(XSetInputFocus(xdisplay, active ? xwindow : None,
        RevertToParent, CurrentTime));
    CHECK(XFlush(xdisplay));
#if GTK_MAJOR_VERSION == 3
    GtkWidget *toplevel = gtk_widget_get_toplevel(input_widget);
#else
    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(input_widget));
#endif
    guint timeout = g_timeout_add_seconds(60, deadline, "X11 focus transition");
    while (gtk_window_is_active(GTK_WINDOW(toplevel)) != active) iterate();
    g_source_remove(timeout);
#elif defined(BOXRIG_TEST_GTK3)
    GdkEvent *event = gdk_event_new(GDK_FOCUS_CHANGE);
    event->focus_change.in = active;
    event->focus_change.window = g_object_ref(gtk_widget_get_window(input_widget));
    gboolean handled;
    g_signal_emit_by_name(input_widget, active ? "focus-in-event" : "focus-out-event", event, &handled);
    gdk_event_free(event);
#else
    g_signal_emit_by_name(focus_controller, active ? "enter" : "leave");
#endif
}

static void key(guint symbol, guint code, GdkModifierType modifiers)
{
#ifdef BOXRIG_TEST_IME_X11
    (void)symbol;
    unsigned int shift = XKeysymToKeycode(xdisplay, GDK_KEY_Shift_L);
    if (modifiers & GDK_SHIFT_MASK) CHECK(XTestFakeKeyEvent(xdisplay, shift, True, CurrentTime));
    CHECK(XTestFakeKeyEvent(xdisplay, code, True, CurrentTime));
    CHECK(XTestFakeKeyEvent(xdisplay, code, False, CurrentTime));
    if (modifiers & GDK_SHIFT_MASK) CHECK(XTestFakeKeyEvent(xdisplay, shift, False, CurrentTime));
    CHECK(XFlush(xdisplay));
#else
    gboolean handled = FALSE;
#ifdef BOXRIG_TEST_GTK3
    for (int release = 0; release < 2; release++) {
        GdkEvent *event = gdk_event_new(release ? GDK_KEY_RELEASE : GDK_KEY_PRESS);
        event->key.window = g_object_ref(gtk_widget_get_window(input_widget));
        event->key.keyval = symbol;
        event->key.hardware_keycode = code;
        event->key.state = modifiers;
        GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(input_widget));
        if (seat != NULL && gdk_seat_get_keyboard(seat) != NULL)
            gdk_event_set_device(event, gdk_seat_get_keyboard(seat));
        g_signal_emit_by_name(input_widget, release ? "key-release-event" : "key-press-event", event, &handled);
        CHECK(handled);
        gdk_event_free(event);
    }
#else
    g_signal_emit_by_name(key_controller, "key-pressed", symbol, code, modifiers, &handled);
    CHECK(handled);
    g_signal_emit_by_name(key_controller, "key-released", symbol, code, modifiers);
#endif
#endif
}

static void preedit(Widget *widget, boxrig_terminal *terminal,
    const char *text, size_t selection, size_t length)
{
    guint timeout = g_timeout_add_seconds(60, deadline, "preedit or cancellation");
    for (;;) {
        frame(widget);
        const uint8_t *bytes;
        /* A function key consumed by the IME must never escape to the PTY. */
        CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
        boxrig_composition_view view = composition(widget);
        if (view.text_length == strlen(text) &&
            (view.text_length == 0 || memcmp(view.text, text, view.text_length) == 0) &&
            view.selection_offset == selection && view.selection_length == length) break;
        iterate();
    }
    g_source_remove(timeout);
}

static void cursor(Widget *widget, IBusBus *bus)
{
    guint timeout = g_timeout_add_seconds(60, deadline, "panel cursor notification");
#ifdef BOXRIG_TEST_IME_X11
    bool reported_wait = false;
#endif
    for (;;) {
        frame(widget);
        boxrig_composition_view view = composition(widget);
        /* OpenGL places the caret during GTK's snapshot. Do not flood the
         * test bus with state queries before the queued drawing completes. */
        if (!view.candidate.visible) {
            iterate();
            continue;
        }
        GError *error = NULL;
        GVariant *reply = g_dbus_connection_call_sync(ibus_bus_get_connection(bus),
            "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "GetState",
            NULL, G_VARIANT_TYPE("(suuuuuiiiiu)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
        CHECK(reply != NULL && error == NULL);
        const char *text;
        guint caret, anchor, caps, presses, releases, notifications;
        gint x, y, width, height;
        g_variant_get(reply, "(&suuuuuiiiiu)", &text, &caret, &anchor, &caps,
            &presses, &releases, &x, &y, &width, &height, &notifications);
        g_variant_unref(reply);
        if (notifications != 0 && view.candidate.visible && width != 0 && height != 0) {
            int scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
#ifdef BOXRIG_TEST_IME_X11
            int root_x, root_y;
            Window child;
            CHECK(XTranslateCoordinates(xdisplay, xwindow, DefaultRootWindow(xdisplay),
                0, 0, &root_x, &root_y, &child));
            double widget_x, widget_y;
#ifdef BOXRIG_TEST_GTK3
            gint px, py;
            CHECK(gtk_widget_translate_coordinates(GTK_WIDGET(widget),
                gtk_widget_get_toplevel(GTK_WIDGET(widget)), 0, 0, &px, &py));
            widget_x = px; widget_y = py;
#else
            GtkNative *native = gtk_widget_get_native(GTK_WIDGET(widget));
            graphene_point_t point;
            CHECK(gtk_widget_compute_point(GTK_WIDGET(widget), GTK_WIDGET(native),
                &GRAPHENE_POINT_INIT(0, 0), &point));
            double dx, dy;
            gtk_native_get_surface_transform(native, &dx, &dy);
            widget_x = point.x + dx; widget_y = point.y + dy;
#endif
            boxrig_gtk_ime_state route;
            CHECK(widget_ime_state(widget, &route) == BOXRIG_UI_OK);
            if (direct_ibus) CHECK(!route.candidate_surface_relative);
            int expected_x = root_x + (int)(widget_x * scale) + (int)view.candidate.exclude_x;
            int expected_y = root_y + (int)(widget_y * scale) + (int)view.candidate.exclude_y;
            /* GTK can round the X root origin to one logical pixel. */
            if (abs(x - expected_x) > scale || abs(y - expected_y) > scale) {
                if (!reported_wait) printf("Await X11 cursor: notified=%d,%d expected=%d,%d count=%u\n",
                    x, y, expected_x, expected_y, notifications);
                reported_wait = true;
                iterate();
                continue;
            }
            CHECK(width == (int)((view.candidate.exclude_width + scale - 1) / scale) * scale);
            CHECK(height == (int)((view.candidate.exclude_height + scale - 1) / scale) * scale);
            printf("X11 IBus rectangle: scale=%d origin=%d,%d widget=%g,%g caret=%zu,%zu notified=%d,%d,%d,%d\n",
                scale, root_x, root_y, widget_x, widget_y, view.candidate.exclude_x,
                view.candidate.exclude_y, x, y, width, height);
#else
#ifdef BOXRIG_TEST_GTK3
            gint px, py;
            GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(widget));
            CHECK(gtk_widget_translate_coordinates(GTK_WIDGET(widget), toplevel,
                view.candidate.exclude_x / scale, view.candidate.exclude_y / scale, &px, &py));
            CHECK(abs(x - px) <= 1);
            CHECK(abs(y - py) <= 1);
#else
            graphene_point_t point;
            GtkNative *native = gtk_widget_get_native(GTK_WIDGET(widget));
            CHECK(gtk_widget_compute_point(GTK_WIDGET(widget), GTK_WIDGET(native),
                &GRAPHENE_POINT_INIT(view.candidate.exclude_x / scale,
                    view.candidate.exclude_y / scale), &point));
            double tx, ty;
            gtk_native_get_surface_transform(native, &tx, &ty);
            /* GTK's standard Wayland IM context adds the native content
             * origin when mapping its widget rectangle into the surface.
             * Nonzero decorations must not be mistaken for an inverse map. */
            CHECK(tx != 0 && ty != 0);
            CHECK(abs(x - (int)(point.x + tx)) <= 1);
            CHECK(abs(y - (int)(point.y + ty)) <= 1);
#endif
            CHECK(width == (int)((view.candidate.exclude_width + scale - 1) / scale));
            CHECK(height == (int)((view.candidate.exclude_height + scale - 1) / scale));
#endif
            break;
        }
        iterate();
    }
    g_source_remove(timeout);
}

static void cursor_changed(GDBusConnection *connection, const char *sender,
    const char *path, const char *interface, const char *signal,
    GVariant *parameters, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    (void)signal; (void)parameters; (void)data;
    request_frame(NULL);
}

static void output(Widget *widget, boxrig_terminal *terminal, const char *expected)
{
    printf("Await GTK IBus output: %s\n", expected);
    guint timeout = g_timeout_add_seconds(60, deadline, "single ordered commit");
    const uint8_t *bytes;
    while (boxrig_terminal_output_peek(terminal, &bytes) < strlen(expected)) {
        frame(widget);
        if (boxrig_terminal_output_peek(terminal, &bytes) < strlen(expected))
            iterate();
    }
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);
    if (length != strlen(expected) || memcmp(bytes, expected, strlen(expected)) != 0) {
        fputs("Observed GTK IBus output:", stderr);
        for (size_t index = 0; index < length; index++) fprintf(stderr, " %02x", bytes[index]);
        fputc('\n', stderr);
    }
    CHECK(length == strlen(expected));
    CHECK(memcmp(bytes, expected, strlen(expected)) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, strlen(expected)) == BOXRIG_STATUS_OK);
    g_source_remove(timeout);
}

#if GTK_MAJOR_VERSION == 4
typedef struct editable_owner {
    char text[64];
    uint64_t revision;
    size_t edits;
    const char *const *expected;
    size_t expected_count;
    unsigned busy;
} editable_owner;

static boxrig_ui_status apply_ordered_edit(
    const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    editable_owner *owner = data;
    CHECK(request->document_identity == 9);
    CHECK(request->base_document_revision == owner->revision);
    if (owner->busy != 0) {
        owner->busy--;
        dispatch_requested = true;
        return BOXRIG_UI_BUSY;
    }
    size_t start = (size_t)request->range.offset, length = strlen(owner->text);
    CHECK(start + request->range.length <= length);
    CHECK(length - request->range.length + request->replacement_length < sizeof(owner->text));
    memmove(owner->text + start + request->replacement_length,
        owner->text + start + request->range.length, length - start - request->range.length + 1);
    if (request->replacement_length != 0)
        memcpy(owner->text + start, request->replacement, request->replacement_length);
    CHECK(owner->edits < owner->expected_count);
    if (strcmp(owner->text, owner->expected[owner->edits]) != 0)
        fprintf(stderr, "Surrounding edit %zu: range=%zu+%zu replacement=%.*s expected=%s observed=%s\n",
            owner->edits, start, request->range.length, (int)request->replacement_length,
            request->replacement == NULL ? "" : request->replacement,
            owner->expected[owner->edits], owner->text);
    CHECK(strcmp(owner->text, owner->expected[owner->edits]) == 0);
    CHECK(proposed->context_length == strlen(owner->text));
    CHECK(memcmp(proposed->context, owner->text, proposed->context_length) == 0);
    owner->revision = proposed->document_revision;
    owner->edits++;
    return BOXRIG_UI_OK;
}

static void ordered_surrounding(Widget *widget, boxrig_terminal *terminal, IBusBus *bus,
    bool unsolicited, bool external_change)
{
    boxrig_ui_editable_document *endpoint = boxrig_ui_view_editable_document(
        boxrig_gtk4_terminal_widget_view(widget));
    const char *gtk_edits[] = {"A🙂前", "A🙂", "A🙂後", "A🙂", "A🙂語"};
    const char *direct_signals[] = {"A🙂前", "A🙂", "A🙂後", "A🙂語"};
    const char *direct_keys[] = {"A🙂後", "A🙂語"};
    const char *external_edits[] = {"外部語"};
    /* Direct IBus applies all edits inside one key reply atomically. Separate
     * unsolicited notifications remain ordered independent transactions. */
    editable_owner owner = {.text = "A🙂", .revision = 1,
        .expected = external_change ? external_edits :
            direct_ibus ? (unsolicited ? direct_signals : direct_keys) : gtk_edits,
        .expected_count = external_change ? 1 : direct_ibus ? (unsolicited ? 4 : 2) : 5,
        .busy = direct_ibus ? 1 : 0};
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint,
        apply_ordered_edit, &owner) == BOXRIG_UI_OK);
    boxrig_ui_editable_context_input context = {9, owner.revision,
        owner.text, strlen(owner.text), 0, strlen(owner.text), strlen(owner.text)};
    CHECK(boxrig_ui_editable_document_publish_context(endpoint, &context) == BOXRIG_UI_OK);
    frame(widget);
    /* The first transaction commits, deletes, then commits. A second native
     * key is already waiting. Every edit must observe the preceding edit,
     * without consuming that key in either input route. */
    if (unsolicited) {
        /* The current context must already process keys, not merely hold them
         * in IBus' asynchronous context-creation queue. Observe its reply. */
        key(GDK_KEY_F1, 67, 0);
        preedit(widget, terminal, "記者の汽車が記者で記者した", 0, 9);
        key(GDK_KEY_Escape, 9, 0);
        preedit(widget, terminal, "", 0, 0);
        boxrig_ui_component_diagnostic diagnostic;
        if (direct_ibus) {
            /* The preedit can clear before Escape's release reply arrives.
             * Finish that transaction before injecting independent signals;
             * otherwise they legitimately join the pending release request. */
            guint previous_timeout = g_timeout_add_seconds(60, deadline, "previous IME key completion");
            for (;;) {
                boxrig_gtk_ime_state state;
                frame(widget);
                CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
                CHECK(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
                if (state.direct.pending == 0 && diagnostic.native_input_queue_depth == 0) break;
                iterate();
            }
            g_source_remove(previous_timeout);
        }
        /* The next native key has arrived, but no filter has sent it to the
         * engine. Independent IM notifications can arrive meanwhile. */
        unsigned expected_f10 = observed_f10 + 1;
        unsigned expected_release = observed_f10_release + 1;
        key(GDK_KEY_F10, 76, 0);
        guint receive_timeout = g_timeout_add_seconds(60, deadline, "queued native key");
        do {
            iterate();
            CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
        } while (observed_f10 < expected_f10 || observed_f10_release < expected_release);
        CHECK(diagnostic.native_input_queue_depth != 0);
        if (direct_ibus) {
            size_t queued = diagnostic.native_input_queue_depth;
            GError *error = NULL;
            GVariant *reply = g_dbus_connection_call_sync(ibus_bus_get_connection(bus),
                "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe",
                "EditSurrounding", NULL, G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE,
                -1, NULL, &error);
            CHECK(reply != NULL && error == NULL);
            g_variant_unref(reply);
            do {
                iterate();
                CHECK(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
            } while (diagnostic.native_input_queue_depth < queued + 3);
            if (external_change) {
                /* Host edits invalidate already received IM transactions.
                 * The retained native key must then edit the new context. */
                strcpy(owner.text, "外部🙂");
                owner.revision++;
                context = (boxrig_ui_editable_context_input){9, owner.revision,
                    owner.text, strlen(owner.text), 0, strlen(owner.text), strlen(owner.text)};
                CHECK(boxrig_ui_editable_document_publish_context(endpoint, &context) == BOXRIG_UI_OK);
            }
        } else {
            g_signal_emit_by_name(observed_im_context, "commit", "前");
            gboolean deleted = FALSE;
            g_signal_emit_by_name(observed_im_context, "delete-surrounding", -1, 1, &deleted);
            CHECK(deleted);
            g_signal_emit_by_name(observed_im_context, "commit", "後");
        }
        g_source_remove(receive_timeout);
    } else {
        key(GDK_KEY_F10, 76, GDK_SHIFT_MASK);
        key(GDK_KEY_F10, 76, 0);
    }
    guint timeout = g_timeout_add_seconds(60, deadline, "ordered surrounding edits");
    const uint8_t *bytes;
    while (owner.edits < owner.expected_count) {
        frame(widget);
        CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
        if (owner.edits < owner.expected_count) iterate();
    }
    g_source_remove(timeout);
    CHECK(strcmp(owner.text, external_change ? "外部語" : "A🙂語") == 0);
    CHECK(boxrig_ui_editable_document_clear(endpoint) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_set_apply_handler(endpoint, NULL, NULL) == BOXRIG_UI_OK);
    puts("PASS GTK 4 native keys preserve commit/delete/commit surrounding transactions");
}
#endif

static void client_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (error != NULL) fprintf(stderr, "IBus client readiness: %s\n", error->message);
    CHECK(reply != NULL && error == NULL);
    const char *name;
    g_variant_get(reply, "(&s)", &name);
    printf("GTK IBus engine focused client: %s\n", name);
    g_variant_unref(reply);
    *(bool *)data = true;
}

static void await_client(Widget *widget, IBusBus *bus)
{
    bool ready = false;
    /* Both input owners create their proxies asynchronously. Observe the real
     * engine focus instead of delaying for an assumed initialization time. */
    const char *prefix = direct_ibus ? (GTK_MAJOR_VERSION == 3 ? "boxrig-gtk3" : "boxrig-gtk4-wayland")
        : GTK_MAJOR_VERSION == 3 ? "gtk3-im:" : "gtk4-im:";
    g_dbus_connection_call(ibus_bus_get_connection(bus), "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe",
        "org.boxrig.IMEProbe", "WaitForClient", g_variant_new("(s)", prefix), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, client_ready, &ready);
    guint timeout = g_timeout_add_seconds(60, deadline, "GTK IBus client readiness");
    while (!ready) { frame(widget); if (!ready) iterate(); }
    g_source_remove(timeout);
}

#ifdef BOXRIG_TEST_IME_X11
static void cursor_at_key(Widget *widget, IBusBus *bus)
{
    /* An engine with its own candidate window records the preedit origin
     * before processing the first key. Updating only after preedit is too late. */
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(ibus_bus_get_connection(bus),
        "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "GetKeyCursor",
        NULL, G_VARIANT_TYPE("(iiii)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    CHECK(reply != NULL && error == NULL);
    gint x, y, width, height;
    g_variant_get(reply, "(iiii)", &x, &y, &width, &height);
    g_variant_unref(reply);
    int root_x, root_y;
    Window child;
    CHECK(XTranslateCoordinates(xdisplay, xwindow, DefaultRootWindow(xdisplay),
        0, 0, &root_x, &root_y, &child));
    double widget_x, widget_y;
#ifdef BOXRIG_TEST_GTK3
    gint px, py;
    CHECK(gtk_widget_translate_coordinates(GTK_WIDGET(widget),
        gtk_widget_get_toplevel(GTK_WIDGET(widget)), 0, 0, &px, &py));
    widget_x = px; widget_y = py;
#else
    GtkNative *native = gtk_widget_get_native(GTK_WIDGET(widget));
    graphene_point_t point;
    CHECK(gtk_widget_compute_point(GTK_WIDGET(widget), GTK_WIDGET(native),
        &GRAPHENE_POINT_INIT(0, 0), &point));
    double dx, dy;
    gtk_native_get_surface_transform(native, &dx, &dy);
    widget_x = point.x + dx; widget_y = point.y + dy;
#endif
    boxrig_composition_view view = composition(widget);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    int expected_x = root_x + (int)(widget_x * scale) + (int)view.candidate.exclude_x;
    int expected_y = root_y + (int)(widget_y * scale) + (int)view.candidate.exclude_y;
    printf("IBus cursor before preedit key: received=%d,%d,%d,%d expected=%d,%d\n",
        x, y, width, height, expected_x, expected_y);
    CHECK(abs(x - expected_x) <= scale && abs(y - expected_y) <= scale);
    CHECK(width == (int)((view.candidate.exclude_width + scale - 1) / scale) * scale);
    CHECK(height == (int)((view.candidate.exclude_height + scale - 1) / scale) * scale);
    boxrig_test_video_hold();
}
#endif

static void received_commit_crosses_input_boundary(Widget *widget, boxrig_terminal *terminal, IBusBus *bus, unsigned boundary)
{
    if (!direct_ibus) return;
    guint timeout = g_timeout_add_seconds(60, deadline, "received commit and focus ordering");
    boxrig_ui_component_diagnostic diagnostic;
    boxrig_gtk_ime_state ime;
    for (;;) {
        frame(widget);
        CHECK(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
        CHECK(widget_ime_state(widget, &ime) == BOXRIG_UI_OK);
        if (diagnostic.native_input_queue_depth == 0 && ime.direct.pending == 0) break;
        iterate();
    }
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(ibus_bus_get_connection(bus),
        "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "CommitText",
        g_variant_new("(s)", "受信済"), G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    CHECK(reply != NULL && error == NULL);
    g_variant_unref(reply);
    /* Receive the engine's completed notification before the native focus
     * loss, but leave applying it to the normal frame dispatch. */
    if (boundary != 2) {
        do {
            iterate();
            CHECK(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
        } while (diagnostic.native_input_queue_depth == 0);
    }
    if (boundary == 0) focus(false);
    else {
        CHECK(widget_set_input_enabled(widget, false) == BOXRIG_UI_OK);
        CHECK(widget_set_input_enabled(widget, true) == BOXRIG_UI_OK);
        await_client(widget, bus);
    }
    for (;;) {
        frame(widget);
        CHECK(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
        if (diagnostic.native_input_queue_depth == 0) break;
        iterate();
    }
    const uint8_t *bytes;
    if (boundary == 0) {
        CHECK(boxrig_terminal_output_peek(terminal, &bytes) == strlen("受信済"));
        CHECK(memcmp(bytes, "受信済", strlen("受信済")) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, strlen("受信済")) == BOXRIG_STATUS_OK);
        focus(true);
    } else CHECK(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    await_client(widget, bus);
    g_source_remove(timeout);
    printf("PASS direct IBus commit crossing input boundary %u\n", boundary);
}


static void background_widget_preserves_input(Widget *focused_widget, const widget_config *config)
{
    boxrig_gtk_ime_state current;
    CHECK(widget_ime_state(focused_widget, &current) == BOXRIG_UI_OK);
    if (current.active_route != BOXRIG_GTK_IME_ROUTE_IBUS) return;
#ifndef BOXRIG_TEST_GTK3
    GtkEventController *saved_key = key_controller, *saved_focus = focus_controller;
    GtkIMContext *saved_context = observed_im_context;
#endif
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    Widget *widget = NULL;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(widget_create(model, config, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
#ifdef BOXRIG_TEST_GTK3
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
#else
    GtkWidget *window = gtk_window_new();
    g_object_ref_sink(window);
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(widget));
#endif
    /* Realization prepares the input backend without presenting or focusing
     * this terminal. It must not take the active terminal's IBus context. */
    gtk_widget_realize(window);
    gtk_widget_realize(GTK_WIDGET(widget));
    boxrig_gtk_ime_state state;
    guint timeout = g_timeout_add_seconds(60, deadline, "background IME initialization");
    for (;;) {
        CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
        if (state.direct.state == BOXRIG_IME_READY && state.direct.pending == 0) break;
        iterate();
    }
    g_source_remove(timeout);
    CHECK(!gtk_widget_has_focus(GTK_WIDGET(widget)));
    CHECK(state.direct.contexts == 0);
    boxrig_composition_view retained = composition(focused_widget);
    CHECK(retained.text_length == strlen("記者の汽車が記者で記者した"));
    CHECK(memcmp(retained.text, "記者の汽車が記者で記者した", retained.text_length) == 0);
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
    key_controller = saved_key;
    focus_controller = saved_focus;
    observed_im_context = saved_context;
#endif
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS an unfocused GTK terminal preserves the active input owner");
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    puts("GTK IBus: initialize display");
#ifdef BOXRIG_TEST_IME_X11
    bool automatic = true;
    bool explicit_direct = argc > 3 && strcmp(argv[3], "direct") == 0;
    direct_ibus = GTK_MAJOR_VERSION == 3 && argc > 3 && strcmp(argv[3], "ibus") != 0;
    if (argc > 3 && strcmp(argv[3], "auto") == 0) CHECK(unsetenv("GTK_IM_MODULE") == 0);
    else if (explicit_direct) CHECK(setenv("GTK_IM_MODULE", "gtk-im-context-simple", 1) == 0);
    else CHECK(setenv("GTK_IM_MODULE", "ibus", 1) == 0);
    /* Match the shipped applications: GTK 4 requires hybrid mode, whereas
     * GTK 3 retains its module's native default (including its key snooper). */
    if (GTK_MAJOR_VERSION == 4) CHECK(setenv("IBUS_ENABLE_SYNC_MODE", "2", 1) == 0);
    else CHECK(unsetenv("IBUS_ENABLE_SYNC_MODE") == 0);
#else
    direct_ibus = true;
    bool automatic = argc > 3 && strcmp(argv[3], "auto") == 0;
    if (automatic) CHECK(unsetenv("GTK_IM_MODULE") == 0);
    else CHECK(setenv("GTK_IM_MODULE", "gtk-im-context-simple", 1) == 0);
#endif
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    IBusBus *bus = ibus_bus_new();
    CHECK(ibus_bus_is_connected(bus));
    guint cursor_subscription = g_dbus_connection_signal_subscribe(ibus_bus_get_connection(bus),
        "org.boxrig.IMEProbe", "org.boxrig.IMEProbe", "CursorChanged", "/org/boxrig/IMEProbe",
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, cursor_changed, NULL, NULL);
    puts("GTK IBus: create widget");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    Widget *widget = NULL;
    widget_config wc = widget_config_default();
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = &dispatch_requested;
    wc.ime_policy = automatic ? BOXRIG_GTK_IME_AUTO : BOXRIG_GTK_IME_IBUS;
#ifdef BOXRIG_TEST_IME_X11
    wc.ime_policy = explicit_direct ? BOXRIG_GTK_IME_IBUS : BOXRIG_GTK_IME_AUTO;
#endif
    wc.render_policy = argc > 2 && strcmp(argv[2], "opengl") == 0
        ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    frame_terminal = terminal;
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    puts("GTK IBus: widget created");
    g_object_ref_sink(widget);
    CHECK(widget_visible(widget, true) == BOXRIG_UI_OK);
    input_widget = GTK_WIDGET(widget);
#ifdef BOXRIG_TEST_GTK3
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
#else
    GtkWidget *window = gtk_window_new();
#endif
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(GTK_WIDGET(widget), 23);
    gtk_widget_set_margin_top(GTK_WIDGET(widget), 17);
    gtk_widget_set_size_request(GTK_WIDGET(widget), 480, 240);
#ifdef BOXRIG_TEST_GTK3
    /* A child GdkWindow and a windowless terminal exercise both origins. */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_set_margin_start(event_box, 31);
    gtk_widget_set_margin_top(event_box, 29);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(widget), TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(event_box), box);
    gtk_box_pack_start(GTK_BOX(outer), event_box, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), outer);
    gtk_widget_show_all(window);
#else
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(widget));
    gtk_window_set_child(GTK_WINDOW(window), box);
#endif
    puts("GTK IBus: present");
    gtk_window_present(GTK_WINDOW(window));
    puts("GTK IBus: await mapping");
    guint timeout = g_timeout_add_seconds(60, deadline, "window mapping");
    while (!gtk_widget_get_mapped(GTK_WIDGET(widget)) ||
        widget_width(GTK_WIDGET(widget)) <= 1 || widget_height(GTK_WIDGET(widget)) <= 1)
        iterate();
    g_source_remove(timeout);
#ifdef BOXRIG_TEST_IME_X11
    boxrig_test_video video = {0};
    CHECK(boxrig_test_video_start(&video, getenv("DISPLAY"), "1600x1200"));
    xdisplay = XOpenDisplay(NULL);
    CHECK(xdisplay != NULL);
#ifdef BOXRIG_TEST_GTK3
    xwindow = gdk_x11_window_get_xid(gtk_widget_get_window(window));
#else
    xwindow = gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window)));
#endif
    CHECK(XMoveWindow(xdisplay, xwindow, 163, 87));
    CHECK(XSetInputFocus(xdisplay, xwindow, RevertToParent, CurrentTime));
    CHECK(XFlush(xdisplay));
    timeout = g_timeout_add_seconds(60, deadline, "X11 native focus");
    while (!gtk_window_is_active(GTK_WINDOW(window))) iterate();
    g_source_remove(timeout);
#endif
    puts("GTK IBus: focus");
    gtk_widget_grab_focus(GTK_WIDGET(widget));
    focus(true);
    frame(widget);
#ifdef BOXRIG_TEST_IME_X11
    if (GTK_MAJOR_VERSION == 4) {
        boxrig_gtk_ime_state state;
        CHECK(widget_ime_state(widget, &state) == BOXRIG_UI_OK);
        CHECK(state.active_route == BOXRIG_GTK_IME_ROUTE_GTK);
        CHECK(strcmp(state.gtk_module, "ibus") == 0);
    }
    await_client(widget, bus);
#endif
    puts("GTK IBus: preedit key");
    key(GDK_KEY_F1, 67, 0);
    preedit(widget, terminal, "記者の汽車が記者で記者した", 0, 9);
    cursor(widget, bus);
#ifdef BOXRIG_TEST_IME_X11
    cursor_at_key(widget, bus);
    const char *positions[] = {"\033[1;12H", "\033[5;7H"};
    for (size_t index = 0; index < G_N_ELEMENTS(positions); index++) {
        key(GDK_KEY_Escape, 9, 0);
        preedit(widget, terminal, "", 0, 0);
        bool queued_key = index == 1 && (direct_ibus || GTK_MAJOR_VERSION == 4);
        if (queued_key) {
            /* A PTY cursor update can arrive after native keys have been
             * queued, but before the next frame filters them through IME. */
            key(GDK_KEY_F1, 67, 0);
            guint queued_timeout = g_timeout_add_seconds(60, deadline, "queued preedit key");
            boxrig_ui_component_diagnostic diagnostic;
            do {
                iterate();
                CHECK(widget_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
            } while (diagnostic.native_input_queue_depth < 2);
            g_source_remove(queued_timeout);
        }
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)positions[index],
            strlen(positions[index]), &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == strlen(positions[index]));
        if (!queued_key) key(GDK_KEY_F1, 67, 0);
        preedit(widget, terminal, "記者の汽車が記者で記者した", 0, 9);
        cursor(widget, bus);
        cursor_at_key(widget, bus);
    }
#endif
    background_widget_preserves_input(widget, &wc);
#ifdef BOXRIG_TEST_IME_X11
    boxrig_gtk_ime_state route;
#if GTK_MAJOR_VERSION == 3
    CHECK(boxrig_gtk3_terminal_widget_get_ime_state(widget, &route) == BOXRIG_UI_OK);
#else
    CHECK(boxrig_gtk4_terminal_widget_get_ime_state(widget, &route) == BOXRIG_UI_OK);
#endif
    CHECK(!route.registry_queried);
    if (direct_ibus) {
        CHECK(route.active_route == BOXRIG_GTK_IME_ROUTE_IBUS);
        CHECK(route.direct.state == BOXRIG_IME_READY && route.direct.contexts == 1);
        CHECK(route.reason == (explicit_direct ? BOXRIG_GTK_IME_REASON_EXPLICIT_IBUS : BOXRIG_GTK_IME_REASON_X11_DIRECT));
    } else {
        CHECK(route.active_route == BOXRIG_GTK_IME_ROUTE_GTK);
        CHECK(route.direct.state == BOXRIG_IME_DISABLED && route.direct.contexts == 0);
        CHECK(strcmp(route.gtk_module, "ibus") == 0);
    }
    CHECK(strcmp(route.display_type, "GdkX11Display") == 0);
    CHECK(route.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_SYSTEM);
    if (GTK_MAJOR_VERSION != 3 || direct_ibus) {
        CHECK(XMoveWindow(xdisplay, xwindow, 347, 193));
        CHECK(XFlush(xdisplay));
        cursor(widget, bus);
    } else puts("Explicit GTK 3 IBus module preserved: upstream same-rectangle cache prevents move tracking");
#endif
    key(GDK_KEY_Right, 114, 0);
    preedit(widget, terminal, "記者の汽車が記者で記者した", 9, 9);
    key(GDK_KEY_Right, 114, GDK_SHIFT_MASK);
    preedit(widget, terminal, "記者の汽車が記者で記者した", 9, 12);
    key(GDK_KEY_Escape, 9, 0);
    preedit(widget, terminal, "", 0, 0);
    key(GDK_KEY_F2, 68, 0);
    preedit(widget, terminal, "A記者の🙂汽車がZ", 14, 9);
    key(GDK_KEY_Return, 36, 0);
    output(widget, terminal, "A記者の🙂汽車がZ");
    preedit(widget, terminal, "", 0, 0);
    key(GDK_KEY_F4, 70, 0);
    preedit(widget, terminal, "A語B", 1, 3);
    key(GDK_KEY_F5, 71, 0);
    preedit(widget, terminal, "", 0, 0);
    key(GDK_KEY_F6, 72, 0);
    preedit(widget, terminal, "A語B", 1, 3);
    key(GDK_KEY_F7, 73, 0);
    preedit(widget, terminal, "", 0, 0);
    key(GDK_KEY_F8, 74, 0);
    output(widget, terminal, "a");
    key(GDK_KEY_a, 38, 0);
    key(GDK_KEY_e, 26, 0);
    output(widget, terminal, "ae");
    key(GDK_KEY_F9, 75, 0);
    preedit(widget, terminal, "確定", 0, 6);
    focus(false);
    output(widget, terminal, "確定");
    focus(true);
    preedit(widget, terminal, "", 0, 0);

    /* An accepted commit held by terminal backpressure must be retried once,
     * with subsequent signals from the same key retaining their order. */
    char *filler = malloc(config.output_capacity);
    CHECK(filler != NULL);
    memset(filler, 'x', config.output_capacity);
    CHECK(boxrig_terminal_send_text(terminal, filler, config.output_capacity) == BOXRIG_STATUS_OK);
    free(filler);
    key(GDK_KEY_F12, 96, 0);
    timeout = g_timeout_add_seconds(60, deadline, "commit backpressure");
    while (!composition(widget).commit_pending) {
        frame(widget);
        if (!composition(widget).commit_pending) iterate();
    }
    g_source_remove(timeout);
    const uint8_t *bytes;
    CHECK(boxrig_terminal_output_peek(terminal, &bytes) == config.output_capacity);
    CHECK(boxrig_terminal_output_consume(terminal, config.output_capacity) == BOXRIG_STATUS_OK);
    output(widget, terminal, "一二");
    preedit(widget, terminal, "", 0, 0);
    for (unsigned boundary = 0; boundary < 3; boundary++)
        received_commit_crosses_input_boundary(widget, terminal, bus, boundary);
#if GTK_MAJOR_VERSION == 4
    ordered_surrounding(widget, terminal, bus, true, false);
    if (direct_ibus) ordered_surrounding(widget, terminal, bus, true, true);
    for (unsigned round = 0; round < 64; round++) {
        printf("Surrounding round %u\n", round);
        ordered_surrounding(widget, terminal, bus, false, false);
    }
#endif
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    g_dbus_connection_signal_unsubscribe(ibus_bus_get_connection(bus), cursor_subscription);
    g_object_unref(bus);
#ifdef BOXRIG_TEST_IME_X11
    CHECK(XCloseDisplay(xdisplay) == 0);
#endif
#ifdef BOXRIG_TEST_IME_X11
    CHECK(boxrig_test_video_stop(&video));
#endif
    printf("PASS GTK %u IBus clauses, cancellation, ordered keys, single commit, focus and candidate coordinates\n", gtk_get_major_version());
    return 0;
}
