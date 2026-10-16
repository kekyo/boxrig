/* boxrig - Under MIT. */
#include "boxrig/gtk4.h"

#include "../../src/ui/accessible.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static size_t snapshot_builds;
boxrig_ui_status __real_boxrig_ui_accessible_snapshot_create(boxrig_ui_view *view,
    size_t maximum_bytes, boxrig_ui_accessible_snapshot **out);
boxrig_ui_status __wrap_boxrig_ui_accessible_snapshot_create(boxrig_ui_view *view,
    size_t maximum_bytes, boxrig_ui_accessible_snapshot **out)
{
    snapshot_builds++;
    return __real_boxrig_ui_accessible_snapshot_create(view, maximum_bytes, out);
}

static char notifications[128];
static size_t notification_count;
static const char *expected_removed, *expected_inserted;
static GtkWindow *release_on_remove;

static void check_bytes(GBytes *bytes, const char *expected)
{
    CHECK(bytes != NULL);
    gsize size;
    const void *data = g_bytes_get_data(bytes, &size);
    CHECK(size == strlen(expected) && memcmp(data, expected, size) == 0);
    g_bytes_unref(bytes);
}

void __real_gtk_accessible_text_update_contents(GtkAccessibleText *text,
    GtkAccessibleTextContentChange change, unsigned int start, unsigned int end);
void __real_gtk_accessible_text_update_caret_position(GtkAccessibleText *text);
void __real_gtk_accessible_text_update_selection_bound(GtkAccessibleText *text);

void __wrap_gtk_accessible_text_update_contents(GtkAccessibleText *text,
    GtkAccessibleTextContentChange change, unsigned int start, unsigned int end)
{
    GtkAccessibleTextInterface *interface = GTK_ACCESSIBLE_TEXT_GET_IFACE(text);
    bool removal = change == GTK_ACCESSIBLE_TEXT_CONTENT_CHANGE_REMOVE;
    const char *expected = removal ? expected_removed : expected_inserted;
    if (expected != NULL) {
        CHECK(end - start == (unsigned int)g_utf8_strlen(expected, -1));
        check_bytes(interface->get_contents(text, start, end), expected);
    }
    boxrig_ui_component_frame_result result;
    CHECK(boxrig_gtk4_terminal_widget_frame(BOXRIG_GTK4_TERMINAL_WIDGET(text), &result) == BOXRIG_UI_BUSY);
    CHECK(boxrig_gtk4_terminal_widget_set_accessibility_enabled(BOXRIG_GTK4_TERMINAL_WIDGET(text), false) == BOXRIG_UI_BUSY);
    notifications[notification_count++] = removal ? 'D' : 'I';
    __real_gtk_accessible_text_update_contents(text, change, start, end);
    if (removal && release_on_remove != NULL) {
        GtkWindow *window = release_on_remove;
        release_on_remove = NULL;
        gtk_window_destroy(window);
        g_object_unref(window);
        /* Drop the application's last reference during a notification. The
         * publishing frame owns the object until all callbacks finish. */
        g_object_unref(text);
    }
}

void __wrap_gtk_accessible_text_update_caret_position(GtkAccessibleText *text)
{
    notifications[notification_count++] = 'C';
    __real_gtk_accessible_text_update_caret_position(text);
}

void __wrap_gtk_accessible_text_update_selection_bound(GtkAccessibleText *text)
{
    notifications[notification_count++] = 'S';
    __real_gtk_accessible_text_update_selection_bound(text);
}

static void frame(BoxrigGtk4TerminalWidget *widget)
{
    boxrig_ui_component_frame_result result;
    CHECK(boxrig_gtk4_terminal_widget_frame(widget, &result) == BOXRIG_UI_OK);
}

static void feed(boxrig_terminal *terminal, BoxrigGtk4TerminalWidget *widget, const char *text)
{
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(text));
    frame(widget);
}

int main(void)
{
    gtk_init();
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 20;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_gtk4_terminal_widget_config widget_config = boxrig_gtk4_terminal_widget_config_default();
    widget_config.ime_policy = BOXRIG_GTK_IME_GTK;
    CHECK(boxrig_gtk4_terminal_widget_create(model, &widget_config, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    g_object_ref_sink(window);
    gtk_window_set_child(window, GTK_WIDGET(widget));
    gtk_window_present(window);
    int width, height;
    gtk_widget_measure(GTK_WIDGET(widget), GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
    gtk_widget_measure(GTK_WIDGET(widget), GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
    gtk_widget_allocate(GTK_WIDGET(widget), width, height, -1, NULL);
    frame(widget);
    CHECK(snapshot_builds == 0);
    CHECK(!widget_config.accessibility_enabled);
    CHECK(!boxrig_gtk4_terminal_widget_get_accessibility_enabled(widget));
    CHECK(boxrig_gtk4_terminal_widget_set_accessibility_enabled(widget, true) == BOXRIG_UI_OK);
    frame(widget);
    CHECK(snapshot_builds == 1);
    CHECK(GTK_IS_ACCESSIBLE_TEXT(widget));
    GtkAccessibleText *text = GTK_ACCESSIBLE_TEXT(widget);
    GtkAccessibleTextInterface *interface = GTK_ACCESSIBLE_TEXT_GET_IFACE(text);
    check_bytes(interface->get_contents(text, 0, G_MAXUINT), "\n\n\n\n");
    notification_count = 0;
    expected_removed = "\n";
    expected_inserted = "A日本é🙂\nline two";
    feed(terminal, widget, "A日本é🙂\r\nline two");
    CHECK(notification_count == 3 && memcmp(notifications, "DIC", 3) == 0);
    check_bytes(interface->get_contents(text, 0, G_MAXUINT), "A日本é🙂\nline two\n\n\n");
    check_bytes(interface->get_contents(text, 1, 5), "日本é");
    CHECK(interface->get_caret_position(text) == 15);
    unsigned int start, end;
    check_bytes(interface->get_contents_at(text, 9, GTK_ACCESSIBLE_TEXT_GRANULARITY_WORD,
        &start, &end), "line ");
    CHECK(start == 7 && end == 12);
    check_bytes(interface->get_contents_at(text, 4, GTK_ACCESSIBLE_TEXT_GRANULARITY_LINE,
        &start, &end), "A日本é🙂\n");
    CHECK(start == 0 && end == 7);
    check_bytes(interface->get_contents(text, 999, G_MAXUINT), "");

    boxrig_ui_view *view = boxrig_gtk4_terminal_widget_view(widget);
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_get_layout(view, &layout) == BOXRIG_UI_OK);
    boxrig_ui_fixed cell_width = layout.grid_width / (boxrig_ui_fixed)layout.columns;
    boxrig_ui_hit_test_result anchor, focus;
    CHECK(boxrig_ui_view_hit_test(view, cell_width, 1, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, cell_width * 5, 1, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    notification_count = 0;
    frame(widget);
    CHECK(notification_count == 1 && notifications[0] == 'S');
    gsize count;
    GtkAccessibleTextRange *ranges = NULL;
    CHECK(interface->get_selection(text, &count, &ranges) && count == 1);
    CHECK(ranges[0].start == 1 && ranges[0].length == 4);
    g_free(ranges);
    CHECK(interface->get_selection(text, &count, NULL) && count == 1);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    frame(widget);
    CHECK(!interface->get_selection(text, &count, &ranges) && count == 0 && ranges == NULL);

    notification_count = 0;
    expected_removed = "line two";
    expected_inserted = "X";
    feed(terminal, widget, "\rX\033[K");
    CHECK(notification_count == 3 && memcmp(notifications, "DIC", 3) == 0);
    CHECK(interface->get_caret_position(text) == 8);
    notification_count = 0;
    const size_t unchanged_builds = snapshot_builds;
    for (size_t i = 0; i < 4; i++) frame(widget);
    CHECK(snapshot_builds == unchanged_builds);
    CHECK(notification_count == 0);

    boxrig_composition *composition = boxrig_gtk4_terminal_widget_composition(widget);
    CHECK(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event preedit = {
        .type = BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        .text = "未確定", .text_length = strlen("未確定"), .caret_visible = true
    };
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_view composition_view;
    CHECK(boxrig_composition_get_view(composition, &composition_view) == BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.active);
    frame(widget);
    CHECK(snapshot_builds == unchanged_builds);
    CHECK(notification_count == 0);
    check_bytes(interface->get_contents(text, 0, G_MAXUINT), "A日本é🙂\nX\n\n\n");
    preedit = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_CANCEL};
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);

    feed(terminal, widget, "\033[?2026h\rY");
    CHECK(notification_count == 0);
    check_bytes(interface->get_contents(text, 7, 8), "X");
    expected_removed = "X";
    expected_inserted = "Y";
    feed(terminal, widget, "\033[?2026l");
    CHECK(notification_count == 2 && memcmp(notifications, "DI", 2) == 0);

    expected_removed = expected_inserted = NULL;
    feed(terminal, widget, "\033[H\033[1;3;4;38;2;1;2;3mA");
    char **names, **values;
    CHECK(interface->get_attributes(text, 0, &count, &ranges, &names, &values));
    bool bold = false, italic = false, underline = false, color = false;
    for (gsize i = 0; i < count; i++) {
        CHECK(ranges[i].start == 0 && ranges[i].length == 1);
        if (strcmp(names[i], "weight") == 0) bold = strcmp(values[i], "700") == 0;
        if (strcmp(names[i], "style") == 0) italic = strcmp(values[i], "italic") == 0;
        if (strcmp(names[i], "underline") == 0) underline = strcmp(values[i], "single") == 0;
        if (strcmp(names[i], "fg-color") == 0) color = strcmp(values[i], "1,2,3") == 0;
    }
    CHECK(bold && italic && underline && color);
    g_strfreev(names); g_strfreev(values); g_free(ranges);
    CHECK(interface->get_attributes(text, 0, &count, NULL, NULL, NULL) && count > 0);
    interface->get_default_attributes(text, &names, &values);
    CHECK(names != NULL && values != NULL);
    g_strfreev(names); g_strfreev(values);
    interface->get_default_attributes(text, NULL, NULL);
    notification_count = 0;
    feed(terminal, widget, "\033[2;1H");
    CHECK(interface->get_caret_position(text) == 7);
    CHECK(notification_count == 1 && notifications[0] == 'C');

    GBytes *retained = interface->get_contents(text, 0, G_MAXUINT);
    void *weak_widget = widget;
    g_object_add_weak_pointer(G_OBJECT(widget), &weak_widget);
    CHECK(boxrig_gtk4_terminal_widget_set_accessibility_enabled(widget, false) == BOXRIG_UI_OK);
    check_bytes(interface->get_contents(text, 0, G_MAXUINT), "");
    const size_t before_disabled = snapshot_builds;
    notification_count = 0;
    feed(terminal, widget, "\033[?2026h\rdisabled update");
    CHECK(snapshot_builds == before_disabled && notification_count == 0);
    check_bytes(interface->get_contents(text, 0, G_MAXUINT), "");
    CHECK(boxrig_gtk4_terminal_widget_set_accessibility_enabled(widget, true) == BOXRIG_UI_OK);
    frame(widget);
    CHECK(snapshot_builds == before_disabled);
    feed(terminal, widget, "\033[?2026l");
    CHECK(snapshot_builds == before_disabled + 1);
    GBytes *latest = interface->get_contents(text, 0, G_MAXUINT);
    CHECK(g_strstr_len(g_bytes_get_data(latest, NULL), (gssize)g_bytes_get_size(latest), "disabled update") != NULL);
    g_bytes_unref(latest);
    notification_count = 0;
    release_on_remove = window;
    feed(terminal, widget, "\033[2J\033[Hdone");
    CHECK(weak_widget == NULL);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    check_bytes(retained, "A日本é🙂\nY\n\n\n");
    puts("PASS GTK4 accessible text, notification snapshots and owner release");
    return 0;
}
