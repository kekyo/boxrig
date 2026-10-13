/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <gtk/gtk.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int exit_status = 1;

typedef struct held_clipboard {
    GtkClipboard *clipboard;
    const char *text;
    gboolean owned;
} held_clipboard;

static void held_clipboard_get(
    GtkClipboard *clipboard,
    GtkSelectionData *selection_data,
    guint info,
    gpointer user_data)
{
    held_clipboard *held = user_data;
    char release;
    ssize_t received;

    (void)clipboard;
    if (info == 1) {
        /* Weston eagerly caches the first offered format. Supply that
         * representation immediately and hold only a consumer's text read. */
        gtk_selection_data_set(
            selection_data,
            gtk_selection_data_get_target(selection_data),
            8,
            (const guchar *)held->text,
            (gint)strlen(held->text));
        return;
    }
    if (fputs("REQUEST\n", stdout) == EOF || fflush(stdout) == EOF) {
        gtk_main_quit();
        return;
    }
    /* Only this external owner is suspended. Its real selection callback
     * cannot reply before the driver releases the pending sample read. */
    do {
        received = read(STDIN_FILENO, &release, 1u);
    } while (received == -1 && errno == EINTR);
    if (received != 1 || release != 'R') {
        gtk_main_quit();
        return;
    }
    gtk_selection_data_set(
        selection_data,
        gtk_selection_data_get_target(selection_data),
        8,
        (const guchar *)held->text,
        (gint)strlen(held->text));
    if (fputs("RELEASED\n", stdout) != EOF && fflush(stdout) != EOF) {
        exit_status = 0;
    }
    gtk_main_quit();
}

static void held_clipboard_clear(GtkClipboard *clipboard, gpointer user_data)
{
    held_clipboard *held = user_data;

    (void)clipboard;
    held->owned = FALSE;
}

static gboolean held_clipboard_key(
    GtkWidget *window,
    GdkEventKey *event,
    gpointer user_data)
{
    static const GtkTargetEntry targets[] = {
        {"application/octet-stream", 0, 1},
        {"UTF8_STRING", 0, 0},
        {"text/plain;charset=utf-8", 0, 0},
        {"text/plain", 0, 0}};
    held_clipboard *held = user_data;

    if (event->keyval != GDK_KEY_c || held->owned) {
        return FALSE;
    }
    held->owned = gtk_clipboard_set_with_data(
        held->clipboard,
        targets,
        G_N_ELEMENTS(targets),
        held_clipboard_get,
        held_clipboard_clear,
        held);
    if (!held->owned) {
        gtk_main_quit();
        return TRUE;
    }
    /* A real key supplies the Wayland input serial. Hide the peer so the
     * driver can select the sample through the native task switcher. */
    gtk_widget_hide(window);
    if (fputs("OWNED\n", stdout) == EOF || fflush(stdout) == EOF) {
        gtk_main_quit();
    }
    return TRUE;
}

static int hold_clipboard(GdkAtom target, const char *text)
{
    held_clipboard held = {gtk_clipboard_get(target), text, FALSE};
    GtkWidget *window;

    if (held.clipboard == NULL || strlen(text) > G_MAXINT) {
        return 1;
    }
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "boxrig-clipboard-peer");
    gtk_window_set_default_size(GTK_WINDOW(window), 240, 120);
    gtk_widget_add_events(window, GDK_KEY_PRESS_MASK);
    g_signal_connect(window, "key-press-event", G_CALLBACK(held_clipboard_key), &held);
    gtk_widget_show(window);
    gtk_main();
    if (held.owned) {
        gtk_clipboard_clear(held.clipboard);
    }
    gtk_widget_destroy(window);
    return exit_status;
}

static GdkAtom clipboard_target(const char *name)
{
    if (strcmp(name, "primary") == 0) {
        return GDK_SELECTION_PRIMARY;
    }
    if (strcmp(name, "clipboard") == 0) {
        return GDK_SELECTION_CLIPBOARD;
    }
    return GDK_NONE;
}

static gboolean owner_input_ready(
    GIOChannel *channel,
    GIOCondition condition,
    gpointer user_data)
{
    char buffer[32];
    gsize bytes_read = 0;
    GError *error = NULL;
    GIOStatus status;

    (void)user_data;
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        exit_status = 0;
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    status = g_io_channel_read_chars(
        channel,
        buffer,
        sizeof(buffer),
        &bytes_read,
        &error);
    if (error != NULL) {
        g_error_free(error);
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    if (status == G_IO_STATUS_EOF) {
        exit_status = 0;
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void clipboard_received(
    GtkClipboard *clipboard,
    GtkSelectionData *selection_data,
    gpointer user_data)
{
    const guchar *bytes;
    gint length;

    (void)clipboard;
    (void)user_data;
    length = gtk_selection_data_get_length(selection_data);
    bytes = gtk_selection_data_get_data(selection_data);
    if (length >= 0 && (length == 0 || bytes != NULL) &&
        fwrite(bytes, 1, (size_t)length, stdout) == (size_t)length &&
        fflush(stdout) == 0) {
        exit_status = 0;
    } else {
        fprintf(stderr, "clipboard contents unavailable (length %d)\n", length);
    }
    gtk_main_quit();
}

static int own_clipboard(GdkAtom target, const char *text)
{
    GtkClipboard *clipboard = gtk_clipboard_get(target);
    GIOChannel *input;

    if (clipboard == NULL) {
        return 1;
    }
    gtk_clipboard_set_text(clipboard, text, -1);
    if (fputs("READY\n", stdout) == EOF || fflush(stdout) == EOF) {
        return 1;
    }
    input = g_io_channel_unix_new(STDIN_FILENO);
    if (input == NULL) {
        return 1;
    }
    g_io_channel_set_close_on_unref(input, FALSE);
    g_io_add_watch(
        input,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        owner_input_ready,
        NULL);
    gtk_main();
    g_io_channel_unref(input);
    return exit_status;
}

static int read_clipboard(GdkAtom target)
{
    GtkClipboard *clipboard = gtk_clipboard_get(target);

    if (clipboard == NULL) {
        return 1;
    }
    gtk_clipboard_request_contents(
        clipboard,
        gdk_atom_intern_static_string("UTF8_STRING"),
        clipboard_received,
        NULL);
    gtk_main();
    return exit_status;
}

static void clipboard_text_received(
    GtkClipboard *clipboard, const gchar *text, gpointer user_data)
{
    (void)clipboard;
    (void)user_data;
    if (text != NULL && fputs(text, stdout) != EOF && fflush(stdout) == 0) {
        exit_status = 0;
    } else {
        fputs("clipboard text unavailable\n", stderr);
    }
    gtk_main_quit();
}

static void read_clipboard_owner(
    GtkClipboard *clipboard, GdkEventOwnerChange *event, gpointer user_data)
{
    gboolean *requested = user_data;
    (void)event;
    if (*requested) return;
    *requested = TRUE;
    /* Negotiate text formats: a clipboard manager may retain only the first
     * offered UTF-8 MIME type after the previous reader disconnects. */
    gtk_clipboard_request_text(
        clipboard, clipboard_text_received, NULL);
}

static int read_active_clipboard(GdkAtom target)
{
    GtkClipboard *clipboard = gtk_clipboard_get(target);
    if (clipboard == NULL) return 1;
    gboolean requested = FALSE;
    /* Wayland announces the offered selection after this client receives
     * keyboard focus. Use that notification instead of a synthetic key,
     * which can reach the previous window during compositor activation. */
    gulong owner_handler = g_signal_connect(clipboard, "owner-change",
        G_CALLBACK(read_clipboard_owner), &requested);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "boxrig-clipboard-reader");
    gtk_window_set_default_size(GTK_WINDOW(window), 240, 120);
    gtk_widget_show(window);
    gtk_main();
    g_signal_handler_disconnect(clipboard, owner_handler);
    gtk_widget_destroy(window);
    return exit_status;
}

int main(int argument_count, char **arguments)
{
    GdkAtom target;

    if (argument_count < 3 || argument_count > 4 ||
        !gtk_init_check(&argument_count, &arguments)) {
        return 2;
    }
    target = clipboard_target(arguments[2]);
    if (target == GDK_NONE) {
        return 2;
    }
    if (strcmp(arguments[1], "get") == 0 && argument_count == 3) {
        return read_clipboard(target);
    }
    if (strcmp(arguments[1], "get-active") == 0 && argument_count == 3) {
        return read_active_clipboard(target);
    }
    if (strcmp(arguments[1], "set") == 0 && argument_count == 4) {
        return own_clipboard(target, arguments[3]);
    }
    if (strcmp(arguments[1], "hold") == 0 && argument_count == 4) {
        return hold_clipboard(target, arguments[3]);
    }
    return 2;
}
