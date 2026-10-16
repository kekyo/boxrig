/* boxrig - Under MIT. */
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION == 3
#include "boxrig/gtk3.h"
typedef BoxrigGtk3TerminalWidget TerminalWidget;
typedef boxrig_gtk3_terminal_widget_config WidgetConfig;
#define API(name) boxrig_gtk3_terminal_widget_##name
#else
#include "boxrig/gtk4.h"
typedef BoxrigGtk4TerminalWidget TerminalWidget;
typedef boxrig_gtk4_terminal_widget_config WidgetConfig;
#define API(name) boxrig_gtk4_terminal_widget_##name
#endif
#include <stdio.h>
#include <string.h>

static boxrig_terminal *terminal;
static TerminalWidget *widget;
static GtkWindow *window;
static GMainLoop *loop;
static guint frame_source;
static bool ready, pending;

static gboolean service_frame(gpointer context)
{
    (void)context;
    frame_source = 0;
    boxrig_ui_layout_result layout;
    if (API(get_layout)(widget, &layout) != BOXRIG_UI_OK) return G_SOURCE_REMOVE;
    boxrig_ui_component_frame_result frame;
    g_assert_cmpint(API(frame)(widget, &frame), ==, BOXRIG_UI_OK);
    if (!ready) {
        ready = true;
        puts("READY");
    }
    if (pending) {
        pending = false;
        puts("ACK");
    }
    fflush(stdout);
    return G_SOURCE_REMOVE;
}

static void request_frame(void *context)
{
    (void)context;
    if (frame_source == 0) frame_source = g_idle_add(service_frame, NULL);
}

static gboolean command(GIOChannel *channel, GIOCondition condition, gpointer context)
{
    (void)context;
    if (condition & G_IO_HUP) {
        g_main_loop_quit(loop);
        return G_SOURCE_CONTINUE;
    }
    char *line = NULL;
    if (g_io_channel_read_line(channel, &line, NULL, NULL, NULL) != G_IO_STATUS_NORMAL)
        return G_SOURCE_CONTINUE;
    boxrig_ui_view *view = API(view)(widget);
    if (line[0] == 'F') {
        gsize length;
        guchar *bytes = g_base64_decode(line + 2, &length);
        size_t consumed;
        g_assert_cmpint(boxrig_terminal_feed(terminal, bytes, length, &consumed), ==, BOXRIG_STATUS_OK);
        g_assert_cmpuint(consumed, ==, length);
        g_free(bytes);
    } else if (line[0] == 'S') {
        boxrig_ui_layout_result layout;
        g_assert_cmpint(API(get_layout)(widget, &layout), ==, BOXRIG_UI_OK);
        boxrig_ui_fixed width = layout.grid_width / (boxrig_ui_fixed)layout.columns;
        boxrig_ui_hit_test_result anchor, focus;
        g_assert_cmpint(boxrig_ui_view_hit_test(view, width, 1, &anchor), ==, BOXRIG_UI_OK);
        g_assert_cmpint(boxrig_ui_view_hit_test(view, width * 5, 1, &focus), ==, BOXRIG_UI_OK);
        g_assert_cmpint(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &anchor), ==, BOXRIG_UI_OK);
        g_assert_cmpint(boxrig_ui_view_update_selection(view, &focus), ==, BOXRIG_UI_OK);
    } else if (line[0] == 'U') {
        g_assert_cmpint(boxrig_ui_view_clear_selection(view), ==, BOXRIG_UI_OK);
    } else if (line[0] == 'T') {
        g_assert_cmpint(boxrig_terminal_set_viewport_offset(terminal, 1), ==, BOXRIG_STATUS_OK);
    } else if (line[0] == 'Z') {
        g_assert_cmpint(boxrig_terminal_resize(terminal, 4, 10), ==, BOXRIG_STATUS_OK);
    } else if (line[0] == 'K') {
        gtk_window_set_focus(window, NULL);
        gtk_widget_grab_focus(GTK_WIDGET(widget));
    } else if (line[0] == 'Q') {
        g_main_loop_quit(loop);
        g_free(line);
        return G_SOURCE_CONTINUE;
    }
    g_assert_cmpint(boxrig_ui_model_synchronize(API(model)(widget)), ==, BOXRIG_UI_OK);
    pending = true;
    request_frame(NULL);
    g_free(line);
    return G_SOURCE_CONTINUE;
}

int main(void)
{
#if GTK_MAJOR_VERSION == 3
    gtk_init(NULL, NULL);
    window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
#else
    gtk_init();
    window = GTK_WINDOW(gtk_window_new());
#endif
    g_object_ref_sink(window);
    gtk_window_set_title(GTK_WINDOW(window), "boxrig-accessibility");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 20;
    config.scrollback_capacity = 100;
    boxrig_ui_model *model = NULL;
    g_assert_cmpint(boxrig_terminal_create(&config, &terminal), ==, BOXRIG_STATUS_OK);
    g_assert_cmpint(boxrig_ui_model_create(terminal, &model), ==, BOXRIG_UI_OK);
    WidgetConfig widget_config = API(config_default)();
    widget_config.accessibility_enabled = true;
    widget_config.frame_request_callback = request_frame;
    widget_config.frame_request_context = &frame_source;
    widget_config.ime_policy = BOXRIG_GTK_IME_GTK;
    g_assert_cmpint(API(create)(model, &widget_config, &widget), ==, BOXRIG_UI_OK);
    g_object_ref_sink(widget);
#if GTK_MAJOR_VERSION == 3
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_show_all(GTK_WIDGET(window));
#else
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(widget));
    gtk_window_present(GTK_WINDOW(window));
#endif
    /* The speech test first observes a complete AT-SPI round trip, then
     * requests focus. GTK4 may emit focus before exporting its D-Bus object. */
    gtk_window_set_focus(window, NULL);
    GIOChannel *input = g_io_channel_unix_new(0);
    guint input_source = g_io_add_watch(input, G_IO_IN | G_IO_HUP, command, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    request_frame(NULL);
    g_main_loop_run(loop);
    g_source_remove(input_source);
    g_io_channel_unref(input);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(GTK_WIDGET(window));
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    if (frame_source != 0) g_source_remove(frame_source);
    g_object_unref(widget);
    g_object_unref(window);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    g_main_loop_unref(loop);
    return 0;
}
