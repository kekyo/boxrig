/* boxrig - Under MIT. */
#include <gtk/gtk.h>
#include <ibus.h>
#if GTK_MAJOR_VERSION == 3
#include "boxrig/gtk3.h"
typedef BoxrigGtk3TerminalWidget terminal_widget;
typedef boxrig_gtk3_terminal_widget_config widget_config;
#define API(name) boxrig_gtk3_terminal_widget_##name
#else
#include "boxrig/gtk4.h"
typedef BoxrigGtk4TerminalWidget terminal_widget;
typedef boxrig_gtk4_terminal_widget_config widget_config;
#define API(name) boxrig_gtk4_terminal_widget_##name
#endif
#include "boxrig/ui/editable.h"
#include <stdio.h>
#include <string.h>
#include "../x11_video.h"

typedef struct driver {
    terminal_widget *widget;
    GtkIMContext *im;
    GtkWidget *window;
    IBusBus *bus;
    GMainLoop *loop;
    boxrig_terminal *terminal;
    boxrig_ui_editable_document *endpoint;
    GString *text;
    GString *preedit;
    const char *mode;
    uint64_t revision;
    uint64_t painted_generation;
    guint next_key;
    unsigned int accepted, rejected, commits, contexts;
    unsigned int japanese_key, preedit_revision, sent_revision;
    gboolean focused, selected, attached, attach_pending, editing, verified, passed;
    gboolean japanese_started;
    boxrig_gtk_render_backend renderer;
} driver;

static driver *current;

static void painted(GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    driver *d = data;
    d->painted_generation = API(presentation_generation)(d->widget);
}

static void observed_commit(GtkIMContext *im, const char *text, gpointer data)
{
    driver *d = data;
    if (!g_str_has_prefix(text, "probe:")) {
        printf("native_commit text=%s\n", text);
        d->commits++;
        if (!d->japanese_started) d->next_key = GDK_KEY_F3;
        return;
    }
    /* Control replies are consumed before the widget's commit handler. All
     * document edits and ordinary commits still use the unmodified widget. */
    g_signal_stop_emission_by_name(im, "commit");
    unsigned int key, caps, cursor, anchor;
    int bytes = 0;
    g_assert_cmpint(sscanf(text, "probe:%u:%u:%u:%u:%n", &key, &caps, &cursor, &anchor, &bytes), ==, 4);
    printf("engine key=%u caps=%u cursor_chars=%u anchor_chars=%u text=%s\n",
        key, caps, cursor, anchor, text + bytes);
    gboolean available = d->attached;
    const char *expected = available ? d->text->str : "";
    unsigned int expected_cursor = !available ? 0 : d->editing && strcmp(d->mode, "empty") == 0 ? 1 :
        strcmp(d->mode, "empty") == 0 ? 0 : d->editing && strcmp(d->mode, "busy") == 0 ? 4 : 3;
    if ((caps & IBUS_CAP_SURROUNDING_TEXT) != (available ? IBUS_CAP_SURROUNDING_TEXT : 0) ||
        strcmp(text + bytes, expected) != 0 || cursor != expected_cursor) {
        d->next_key = d->editing ? GDK_KEY_F3 : GDK_KEY_F1;
        return;
    }
    g_assert_cmpuint(anchor, ==, cursor);
    if (!d->attached && strcmp(d->mode, "attach") == 0) {
        d->attach_pending = TRUE;
        return;
    }
    if (!d->editing) {
        d->editing = TRUE;
        if (g_str_has_prefix(d->mode, "mozc")) {
            g_assert_true(ibus_bus_set_global_engine(d->bus, "mozc-jp"));
            d->japanese_started = TRUE;
            return;
        }
        d->next_key = strcmp(d->mode, "empty") == 0 || strcmp(d->mode, "none") == 0 ||
            strcmp(d->mode, "busy") == 0
            ? GDK_KEY_F4 : GDK_KEY_F2;
        return;
    }
    d->verified = TRUE;
}

static void observed_preedit(GtkIMContext *im, gpointer data)
{
    driver *d = data;
    char *text = NULL;
    PangoAttrList *attributes = NULL;
    int cursor;
    gtk_im_context_get_preedit_string(im, &text, &attributes, &cursor);
    g_string_assign(d->preedit, text);
    d->preedit_revision++;
    printf("native_preedit revision=%u cursor=%d text=%s\n", d->preedit_revision, cursor, text);
    g_free(text);
    if (attributes != NULL) pango_attr_list_unref(attributes);
}

GtkIMContext *__real_gtk_im_multicontext_new(void);
/** Captures each real context, including recovery after attaching a document. */
GtkIMContext *__wrap_gtk_im_multicontext_new(void)
{
    GtkIMContext *context = __real_gtk_im_multicontext_new();
    current->im = context;
    current->selected = FALSE;
    current->contexts++;
    g_signal_connect(context, "commit", G_CALLBACK(observed_commit), current);
    g_signal_connect(context, "preedit-changed", G_CALLBACK(observed_preedit), current);
    return context;
}

static boxrig_ui_status apply(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    driver *d = data;
    printf("owner offset=%" G_GUINT64_FORMAT " length=%zu replacement=%.*s\n",
        request->range.offset, request->range.length, (int)request->replacement_length, request->replacement);
    g_assert_cmpuint(request->base_document_revision, ==, d->revision);
    if (strcmp(d->mode, "reject") == 0) {
        d->rejected++;
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (strcmp(d->mode, "busy") == 0 && d->rejected == 0) {
        d->rejected++;
        return BOXRIG_UI_BUSY;
    }
    g_string_erase(d->text, (gssize)request->range.offset, (gssize)request->range.length);
    g_string_insert_len(d->text, (gssize)request->range.offset, request->replacement,
        (gssize)request->replacement_length);
    g_assert_cmpuint(d->text->len, ==, proposed->context_length);
    g_assert_cmpint(memcmp(d->text->str, proposed->context, d->text->len), ==, 0);
    d->revision = proposed->document_revision;
    d->accepted++;
    return BOXRIG_UI_OK;
}

static void attach(driver *d)
{
    d->revision = 1;
    d->attached = TRUE;
    if (strcmp(d->mode, "readonly") != 0)
        g_assert_cmpint(boxrig_ui_editable_document_set_apply_handler(d->endpoint, apply, d), ==, BOXRIG_UI_OK);
    uint64_t cursor = d->text->len == 0 ? 0 : 9;
    boxrig_ui_editable_context_input input = {7, d->revision, d->text->str, d->text->len, 0, cursor, cursor};
    g_assert_cmpint(boxrig_ui_editable_document_publish_context(d->endpoint, &input), ==, BOXRIG_UI_OK);
    d->next_key = GDK_KEY_F1;
}

static gboolean advance(gpointer data)
{
    driver *d = data;
    if (!gtk_widget_get_mapped(GTK_WIDGET(d->widget))) return G_SOURCE_CONTINUE;
    boxrig_ui_layout_result layout;
    if (API(get_layout)(d->widget, &layout) != BOXRIG_UI_OK) return G_SOURCE_CONTINUE;
    if (d->attach_pending) {
        d->attach_pending = FALSE;
        attach(d);
    }
    boxrig_ui_component_frame_result result;
    g_assert_cmpint(API(frame)(d->widget, &result), ==, BOXRIG_UI_OK);
    boxrig_ui_layout_action action;
    while (API(take_layout_action)(d->widget, &action)) {
        g_assert_cmpint(action.kind, ==, BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        g_assert_cmpint(boxrig_terminal_resize(d->terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns), ==, BOXRIG_STATUS_OK);
        g_assert_cmpint(API(complete_grid_resize)(d->widget, action.revision, true), ==, BOXRIG_UI_OK);
    }
    if (!d->focused) {
        gtk_widget_grab_focus(GTK_WIDGET(d->widget));
        gtk_im_context_focus_in(d->im);
        d->focused = TRUE;
    }
    /* The GTK module initializes IBus. Repeating ibus_init() here registers
     * its static resource twice in IBus 1.5.29-rc2. */
    if (d->bus == NULL) d->bus = ibus_bus_new();
    if (!d->selected) {
        gchar *path = ibus_bus_current_input_context(d->bus);
        if (path == NULL) return G_SOURCE_CONTINUE;
        g_free(path);
        if (!ibus_bus_set_global_engine(d->bus, "boxrig-surrounding-probe")) return G_SOURCE_CONTINUE;
        d->selected = TRUE;
    }
    if (d->verified) {
        boxrig_gtk_render_state state;
        g_assert_cmpint(API(get_render_state)(d->widget, &state), ==, BOXRIG_UI_OK);
        if (state.pending || state.active_backend != d->renderer ||
            API(presentation_generation)(d->widget) == 0) return G_SOURCE_CONTINUE;
        gboolean no_document = !d->attached;
        gboolean empty = strcmp(d->mode, "empty") == 0;
        gboolean busy = strcmp(d->mode, "busy") == 0;
        gboolean refused = strcmp(d->mode, "reject") == 0 || strcmp(d->mode, "readonly") == 0;
        g_assert_cmpstr(d->text->str, ==, no_document || refused ? "前かな後" :
            d->japanese_started ? "前かな日本語後" : empty ? "語" : busy ? "前かな語後" : "前か語後");
        g_assert_cmpuint(d->accepted, ==, no_document || refused ? 0 : empty || busy || d->japanese_started ? 1 : 2);
        g_assert_cmpuint(d->rejected, ==, strcmp(d->mode, "reject") == 0 || busy ? 1 : 0);
        const uint8_t *output;
        size_t length = boxrig_terminal_output_peek(d->terminal, &output);
        const char *committed = d->japanese_started ? "日本語" : "語";
        g_assert_cmpuint(length, ==, no_document ? strlen(committed) : 0);
        if (no_document) g_assert_cmpint(memcmp(output, committed, length), ==, 0);
        printf("PASS widget gtk=%d mode=%s renderer=%d contexts=%u document=%s pty_bytes=%zu\n",
            GTK_MAJOR_VERSION, d->mode, (int)state.active_backend, d->contexts, d->text->str, length);
        d->passed = TRUE;
        g_main_loop_quit(d->loop);
        return G_SOURCE_CONTINUE;
    }
    static const guint japanese_keys[] = {'n', 'i', 'h', 'o', 'n', 'g', 'o',
        GDK_KEY_space, GDK_KEY_Return, 'a', GDK_KEY_Escape};
    static const guint japanese_codes[] = {57, 31, 43, 32, 57, 42, 32, 65, 36, 38, 9};
    if (d->japanese_started) {
        if (d->painted_generation == 0 ||
            d->painted_generation != API(presentation_generation)(d->widget)) return G_SOURCE_CONTINUE;
        if (d->japanese_key == 9) {
            if (d->commits == 0) return G_SOURCE_CONTINUE;
            g_assert_cmpuint(d->commits, ==, 1);
            /* IBus 1.5.29-rc2's default GTK 4 synchronous path cannot deliver
             * hide-preedit ('h'). The default-mode case checks conversion;
             * the documented hybrid mode also checks native cancellation. */
            if (g_str_has_prefix(d->mode, "mozc-default")) {
                d->verified = TRUE;
                return G_SOURCE_CONTINUE;
            }
        } else if (d->japanese_key != 0 && d->preedit_revision == d->sent_revision) {
            return G_SOURCE_CONTINUE;
        }
        if (d->japanese_key == 7) g_assert_cmpstr(d->preedit->str, ==, "にほんご");
        if (d->japanese_key == 8) g_assert_cmpstr(d->preedit->str, ==, "日本語");
        if (d->japanese_key == 10) g_assert_cmpstr(d->preedit->str, ==, "あ");
        if (d->japanese_key == G_N_ELEMENTS(japanese_keys)) {
            g_assert_cmpstr(d->preedit->str, ==, "");
            g_assert_cmpuint(d->commits, ==, 1);
            boxrig_composition_view composition;
            g_assert_cmpint(boxrig_composition_get_view(API(composition)(d->widget), &composition), ==,
                BOXRIG_COMPOSITION_OK);
            g_assert_false(composition.active);
            d->verified = TRUE;
            boxrig_test_video_hold();
            return G_SOURCE_CONTINUE;
        }
        boxrig_test_video_hold();
        d->next_key = japanese_keys[d->japanese_key];
    }
    if (d->next_key == 0) return G_SOURCE_CONTINUE;
    guint key = d->next_key;
    gboolean japanese_key_sent = d->japanese_started;
    guint keycode = d->japanese_started ? japanese_codes[d->japanese_key] : 67 + key - GDK_KEY_F1;
    d->next_key = 0;
    d->sent_revision = d->preedit_revision;
    if (d->japanese_started) printf("japanese_key index=%u value=%u code=%u\n", d->japanese_key, key, keycode);
    GdkDevice *keyboard = gdk_seat_get_keyboard(gdk_display_get_default_seat(
        gtk_widget_get_display(GTK_WIDGET(d->widget))));
#if GTK_MAJOR_VERSION == 3
    GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
    event->key.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(d->widget)));
    event->key.keyval = key;
    event->key.hardware_keycode = (guint16)keycode;
    gdk_event_set_device(event, keyboard);
    gboolean handled = gtk_im_context_filter_keypress(d->im, &event->key);
    gdk_event_free(event);
#else
    gboolean handled = gtk_im_context_filter_key(d->im, TRUE,
        gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(d->widget))), keyboard,
        GDK_CURRENT_TIME, keycode, 0, 0);
#endif
    if (!handled && (key == GDK_KEY_F1 || key == GDK_KEY_F3)) d->next_key = key;
    else g_assert_true(handled);
    if (japanese_key_sent) d->japanese_key++;
    return G_SOURCE_CONTINUE;
}

static gboolean timeout(gpointer data)
{
    driver *d = data;
    fprintf(stderr, "FAIL timeout mode=%s contexts=%u accepted=%u rejected=%u commits=%u\n",
        d->mode, d->contexts, d->accepted, d->rejected, d->commits);
    g_main_loop_quit(d->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    driver d = {.mode = argv[1], .next_key = GDK_KEY_F1};
    current = &d;
    setvbuf(stdout, NULL, _IONBF, 0);
    boxrig_test_video video = {0};
    g_assert_true(boxrig_test_video_start(&video, getenv("DISPLAY"), "1280x1024"));
#if GTK_MAJOR_VERSION == 3
    gtk_init(NULL, NULL);
    d.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    gtk_init();
    d.window = gtk_window_new();
#endif
    d.loop = g_main_loop_new(NULL, FALSE);
    d.text = g_string_new(strcmp(d.mode, "empty") == 0 ? "" : "前かな後");
    d.preedit = g_string_new("");
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_model *model = NULL;
    g_assert_cmpint(boxrig_terminal_create(&tc, &d.terminal), ==, BOXRIG_STATUS_OK);
    g_assert_cmpint(boxrig_ui_model_create(d.terminal, &model), ==, BOXRIG_UI_OK);
    widget_config config = API(config_default)();
    gboolean opengl = strcmp(argv[2], "opengl") == 0;
    config.render_policy = opengl ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO;
    d.renderer = opengl ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO;
    g_assert_cmpint(API(create)(model, &config, &d.widget), ==, BOXRIG_UI_OK);
    g_object_ref_sink(d.widget);
    d.endpoint = boxrig_ui_view_editable_document(API(view)(d.widget));
    if (strcmp(d.mode, "none") != 0 && !g_str_has_suffix(d.mode, "-none") && strcmp(d.mode, "attach") != 0)
        attach(&d);
#if GTK_MAJOR_VERSION == 3
    gtk_container_add(GTK_CONTAINER(d.window), GTK_WIDGET(d.widget));
    gtk_widget_show_all(d.window);
#else
    gtk_window_set_child(GTK_WINDOW(d.window), GTK_WIDGET(d.widget));
    gtk_window_present(GTK_WINDOW(d.window));
#endif
    GdkFrameClock *clock = gtk_widget_get_frame_clock(d.window);
    g_assert_nonnull(clock);
    gulong painted_handler = g_signal_connect_after(clock, "after-paint", G_CALLBACK(painted), &d);
    guint advance_source = g_timeout_add(10, advance, &d);
    guint timeout_source = g_timeout_add_seconds(30, timeout, &d);
    g_main_loop_run(d.loop);
    g_source_remove(advance_source);
    if (d.passed) g_source_remove(timeout_source);
    g_signal_handler_disconnect(clock, painted_handler);
    g_assert_true(boxrig_test_video_stop(&video));
    g_assert_cmpint(API(quiesce)(d.widget), ==, BOXRIG_UI_OK);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(d.window);
#else
    gtk_window_destroy(GTK_WINDOW(d.window));
#endif
    g_object_unref(d.widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(d.terminal);
    g_clear_object(&d.bus);
    g_main_loop_unref(d.loop);
    g_string_free(d.text, TRUE);
    g_string_free(d.preedit, TRUE);
    return d.passed ? 0 : 1;
}
