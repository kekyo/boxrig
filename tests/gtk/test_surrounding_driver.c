/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <gtk/gtk.h>
#include <ibus.h>
#include "boxrig/terminal.h"
#include <stdio.h>
#include <string.h>

typedef struct driver {
    GtkWidget *window;
    GtkWidget *widget;
    GtkIMContext *im;
    IBusBus *bus;
    GMainLoop *loop;
    const char *mode;
    GString *document;
    GString *output;
    boxrig_terminal *terminal;
    int cursor;
    int anchor;
    unsigned int retrieved;
    unsigned int deleted;
    unsigned int committed;
    unsigned int next_key;
    gboolean engine_selected;
    gboolean focused;
    gboolean passed;
} driver;

static void feed(driver *d, const char *text)
{
    size_t consumed = 0;
    g_assert_cmpint(boxrig_terminal_feed(d->terminal, (const uint8_t *)text,
        strlen(text), &consumed), ==, BOXRIG_STATUS_OK);
    g_assert_cmpuint(consumed, ==, strlen(text));
}

static void read_screen(driver *d, size_t columns)
{
    boxrig_text_range range;
    boxrig_text_position cursor;
    boxrig_cursor_view native_cursor;
    boxrig_terminal_get_cursor(d->terminal, &native_cursor);
    g_assert_cmpint(boxrig_terminal_get_text_position(d->terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, 0, &range.start), ==, BOXRIG_STATUS_OK);
    g_assert_cmpint(boxrig_terminal_get_text_position(d->terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, columns, &range.end), ==, BOXRIG_STATUS_OK);
    g_assert_cmpint(boxrig_terminal_get_text_position(d->terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, native_cursor.column, &cursor), ==,
        BOXRIG_STATUS_OK);
    boxrig_text_snapshot_options options = {
        BOXRIG_TEXT_SCREEN_PRIMARY, &range, 128, 1};
    boxrig_text_snapshot *snapshot = NULL;
    g_assert_cmpint(boxrig_terminal_create_text_snapshot(d->terminal, &options,
        &snapshot), ==, BOXRIG_STATUS_OK);
    size_t length = 0, cursor_offset = 0;
    const uint8_t *text = boxrig_text_snapshot_data(snapshot, &length);
    g_assert_cmpint(boxrig_text_snapshot_byte_offset_at_position(snapshot,
        &cursor, &cursor_offset), ==, BOXRIG_STATUS_OK);
    g_string_assign(d->document, "");
    g_string_append_len(d->document, (const char *)text, (gssize)length);
    d->cursor = d->anchor = (int)cursor_offset;
    boxrig_text_snapshot_destroy(snapshot);
}

static gboolean retrieve(GtkIMContext *im, gpointer data)
{
    driver *d = data;
    d->retrieved++;
    if (strcmp(d->mode, "none") == 0) return FALSE;
    const char *text = strcmp(d->mode, "empty") == 0 ? "" : d->document->str;
    int cursor = strcmp(d->mode, "empty") == 0 ? 0 : d->cursor;
    int anchor = strcmp(d->mode, "empty") == 0 ? 0 : d->anchor;
#if GTK_MAJOR_VERSION == 3
    (void)anchor;
    gtk_im_context_set_surrounding(im, text, -1, cursor);
#else
    gtk_im_context_set_surrounding_with_selection(im, text, -1, cursor, anchor);
#endif
    printf("retrieve text=%s cursor_bytes=%d anchor_bytes=%d\n", text,
        cursor, anchor);
    return TRUE;
}

static gboolean delete_surrounding(
    GtkIMContext *im, gint offset, gint count, gpointer data)
{
    (void)im;
    driver *d = data;
    d->deleted++;
    gboolean accepted = strcmp(d->mode, "editable") == 0;
    printf("delete offset_chars=%d count_chars=%d accepted=%d\n",
        offset, count, accepted);
    g_assert_cmpint(offset, ==, -1);
    g_assert_cmpint(count, ==, 1);
    if (!accepted) return FALSE;
    const char *start = g_utf8_offset_to_pointer(d->document->str + d->cursor,
        offset);
    const char *end = g_utf8_offset_to_pointer(start, count);
    int first = (int)(start - d->document->str);
    g_string_erase(d->document, first, end - start);
    d->cursor = d->anchor = first;
    return TRUE;
}

static void commit(GtkIMContext *im, const gchar *text, gpointer data)
{
    (void)im;
    driver *d = data;
    if (g_str_has_prefix(text, "probe:")) {
        unsigned int key, caps, cursor, anchor;
        int bytes = 0;
        g_assert_cmpint(sscanf(text, "probe:%u:%u:%u:%u:%n",
            &key, &caps, &cursor, &anchor, &bytes), ==, 4);
        if (key == 1 && d->retrieved == 0) {
            d->next_key = GDK_KEY_F1;
            return;
        }
        printf("engine key=%u caps=%u cursor_chars=%u anchor_chars=%u text=%s\n",
            key, caps, cursor, anchor, text + bytes);
        gboolean unavailable = strcmp(d->mode, "none") == 0;
        gboolean empty = strcmp(d->mode, "empty") == 0;
        /* SetCapabilities and SetSurroundingText are asynchronous. Probe until
         * the first published state reaches the engine, before editing it. */
        if (key == 1 &&
            ((caps & IBUS_CAP_SURROUNDING_TEXT) !=
                 (unavailable ? 0 : IBUS_CAP_SURROUNDING_TEXT) ||
             strcmp(text + bytes, unavailable || empty ? "" : "前かな後") != 0)) {
            d->next_key = GDK_KEY_F1;
            return;
        }
        g_assert_cmpuint(caps & IBUS_CAP_SURROUNDING_TEXT, ==,
            unavailable ? 0 : IBUS_CAP_SURROUNDING_TEXT);
        if (key == 1) {
            g_assert_cmpstr(text + bytes, ==,
                unavailable || empty ? "" : "前かな後");
            g_assert_cmpuint(cursor, ==, unavailable || empty ? 0 : 3);
            /* IBus 1.5.29 ignores the supplied anchor on custom widgets. */
            g_assert_cmpuint(anchor, ==, cursor);
            d->next_key = GDK_KEY_F2;
        } else {
            g_assert_cmpuint(d->deleted, ==, 1);
            g_assert_cmpuint(d->committed, ==, 1);
            if (strcmp(d->mode, "editable") == 0) {
                g_assert_cmpstr(d->document->str, ==, "前か語後");
                g_assert_cmpstr(d->output->str, ==, "");
                g_assert_cmpstr(text + bytes, ==, d->document->str);
                g_assert_cmpuint(cursor, ==, 3);
            } else {
                g_assert_cmpstr(d->document->str, ==, "前かな後");
                g_assert_cmpstr(d->output->str, ==, "語");
                if (strcmp(d->mode, "screen") == 0) {
                    /* The engine changed its cache despite our rejection;
                     * an unchanged screen snapshot does not repair it. */
                    g_assert_cmpstr(text + bytes, ==, "前か後");
                    g_assert_cmpuint(cursor, ==, 2);
                    feed(d, "\033[4h");
                    feed(d, d->output->str);
                    read_screen(d, 10);
                    g_assert_cmpstr(d->document->str, ==, "前かな語後");
                    printf("echo screen=%s cursor_bytes=%d\n",
                        d->document->str, d->cursor);
                }
            }
            printf("PASS gtk=%d mode=%s document=%s output=%s\n",
                GTK_MAJOR_VERSION, d->mode, d->document->str, d->output->str);
            d->passed = TRUE;
            g_main_loop_quit(d->loop);
        }
        return;
    }
    printf("commit text=%s\n", text);
    g_assert_cmpstr(text, ==, "語");
    d->committed++;
    if (strcmp(d->mode, "editable") == 0) {
        g_string_insert(d->document, d->cursor, text);
        d->cursor += (int)strlen(text);
        d->anchor = d->cursor;
    } else {
        /* Hold the echo until after the second engine observation. */
        g_assert_cmpint(boxrig_terminal_send_text(d->terminal, text,
            strlen(text)), ==, BOXRIG_STATUS_OK);
        const uint8_t *bytes = NULL;
        size_t length = boxrig_terminal_output_peek(d->terminal, &bytes);
        g_string_append_len(d->output, (const char *)bytes, (gssize)length);
        g_assert_cmpint(boxrig_terminal_output_consume(d->terminal, length),
            ==, BOXRIG_STATUS_OK);
    }
    d->next_key = GDK_KEY_F3;
}

static gboolean advance(gpointer data)
{
    driver *d = data;
    if (!gtk_widget_get_mapped(d->widget)) return G_SOURCE_CONTINUE;
    if (!d->focused) {
        gtk_widget_grab_focus(d->widget);
        gtk_im_context_focus_in(d->im);
        d->focused = TRUE;
    }
    /* The GTK module owns IBus initialization, including its static resource. */
    if (d->bus == NULL) d->bus = ibus_bus_new();
    if (!d->engine_selected) {
        gchar *path = ibus_bus_current_input_context(d->bus);
        if (path == NULL) return G_SOURCE_CONTINUE;
        printf("select_engine context=%s\n", path);
        g_free(path);
        if (!ibus_bus_set_global_engine(d->bus, "boxrig-surrounding-probe"))
            return G_SOURCE_CONTINUE;
        d->engine_selected = TRUE;
    }
    if (d->next_key == 0) return G_SOURCE_CONTINUE;
    guint key = d->next_key;
    d->next_key = 0;
    GdkDevice *keyboard = gdk_seat_get_keyboard(
        gdk_display_get_default_seat(gtk_widget_get_display(d->widget)));
#if GTK_MAJOR_VERSION == 3
    GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
    event->key.window = g_object_ref(gtk_widget_get_window(d->widget));
    event->key.keyval = key;
    event->key.hardware_keycode = (guint16)(67 + key - GDK_KEY_F1);
    gdk_event_set_device(event, keyboard);
    gboolean handled = gtk_im_context_filter_keypress(d->im, &event->key);
    gdk_event_free(event);
#else
    gboolean handled = gtk_im_context_filter_key(d->im, TRUE,
        gtk_native_get_surface(gtk_widget_get_native(d->widget)), keyboard,
        GDK_CURRENT_TIME, 67 + key - GDK_KEY_F1, 0, 0);
#endif
    if (!handled && key == GDK_KEY_F1) d->next_key = key;
    else g_assert_true(handled);
    return G_SOURCE_CONTINUE;
}

static gboolean timeout(gpointer data)
{
    driver *d = data;
    fprintf(stderr, "FAIL timed out mode=%s retrieved=%u deleted=%u commits=%u\n",
        d->mode, d->retrieved, d->deleted, d->committed);
    g_main_loop_quit(d->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    driver d = {0};
    d.mode = argv[1];
    d.document = g_string_new("前かな後");
    d.output = g_string_new("");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 20;
    g_assert_cmpint(boxrig_terminal_create(&config, &d.terminal), ==,
        BOXRIG_STATUS_OK);
    d.cursor = 9;
    d.anchor = strcmp(d.mode, "editable") == 0 ? 3 : 9;
    if (strcmp(d.mode, "screen") == 0) {
        feed(&d, "前かな後\033[1;7H");
        read_screen(&d, 8);
    }
    d.next_key = GDK_KEY_F1;
    setvbuf(stdout, NULL, _IONBF, 0);
#if GTK_MAJOR_VERSION == 3
    gtk_init(&argc, &argv);
    d.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    d.widget = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(d.window), d.widget);
    gtk_widget_show_all(d.window);
#else
    gtk_init();
    d.window = gtk_window_new();
    d.widget = gtk_drawing_area_new();
    gtk_window_set_child(GTK_WINDOW(d.window), d.widget);
    gtk_window_present(GTK_WINDOW(d.window));
#endif
    d.loop = g_main_loop_new(NULL, FALSE);
    d.im = gtk_im_multicontext_new();
    gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(d.im), "ibus");
    g_signal_connect(d.im, "retrieve-surrounding", G_CALLBACK(retrieve), &d);
    g_signal_connect(d.im, "delete-surrounding", G_CALLBACK(delete_surrounding), &d);
    g_signal_connect(d.im, "commit", G_CALLBACK(commit), &d);
#if GTK_MAJOR_VERSION == 3
    gtk_im_context_set_client_window(d.im, gtk_widget_get_window(d.widget));
#else
    gtk_im_context_set_client_widget(d.im, d.widget);
#endif
#if GTK_MAJOR_VERSION == 3
    gtk_widget_set_can_focus(d.widget, TRUE);
#else
    gtk_widget_set_focusable(d.widget, TRUE);
#endif
    guint advance_source = g_timeout_add(10, advance, &d);
    guint timeout_source = g_timeout_add_seconds(30, timeout, &d);
    g_main_loop_run(d.loop);
    g_source_remove(advance_source);
    if (d.passed) g_source_remove(timeout_source);
    gtk_im_context_focus_out(d.im);
    g_object_unref(d.im);
    g_clear_object(&d.bus);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(d.window);
#else
    gtk_window_destroy(GTK_WINDOW(d.window));
#endif
    g_main_loop_unref(d.loop);
    g_string_free(d.document, TRUE);
    g_string_free(d.output, TRUE);
    boxrig_terminal_destroy(d.terminal);
    return d.passed ? 0 : 1;
}
