/* boxrig - Under MIT. */
#include "boxrig/gtk3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static char notifications[128];
static size_t notification_count;
static GtkWidget *destroy_on_remove;

static void removed(AtkText *accessible, int start, int count, const char *text, void *data)
{
    boxrig_ui_component_frame_result result;
    CHECK(boxrig_gtk3_terminal_widget_frame(data, &result) == BOXRIG_UI_BUSY);
    char *observed = atk_text_get_text(accessible, start, start + count);
    CHECK(strcmp(observed, text) == 0);
    CHECK(g_utf8_strlen(text, -1) == count);
    g_free(observed);
    notifications[notification_count++] = 'D';
    if (destroy_on_remove != NULL) {
        GtkWidget *widget = destroy_on_remove;
        destroy_on_remove = NULL;
        gtk_widget_destroy(widget);
    }
}

static void inserted(AtkText *accessible, int start, int count, const char *text, void *data)
{
    (void)data;
    char *observed = atk_text_get_text(accessible, start, start + count);
    CHECK(strcmp(observed, text) == 0);
    CHECK(g_utf8_strlen(text, -1) == count);
    g_free(observed);
    notifications[notification_count++] = 'I';
}

static void caret_changed(AtkText *accessible, int offset, void *data)
{
    (void)data;
    CHECK(atk_text_get_caret_offset(accessible) == offset);
    notifications[notification_count++] = 'C';
}

static void selection_changed(AtkText *accessible, void *data)
{
    (void)accessible; (void)data;
    notifications[notification_count++] = 'S';
}

static void frame(BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_component_frame_result result;
    CHECK(boxrig_gtk3_terminal_widget_frame(widget, &result) == BOXRIG_UI_OK);
}

static void feed(boxrig_terminal *terminal, BoxrigGtk3TerminalWidget *widget, const char *text)
{
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK && consumed == strlen(text));
    frame(widget);
}

int main(void)
{
    gtk_init(NULL, NULL);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 20;
    config.scrollback_capacity = 100;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_gtk3_terminal_widget_config widget_config = boxrig_gtk3_terminal_widget_config_default();
    CHECK(boxrig_gtk3_terminal_widget_create(model, &widget_config, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(window);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    gtk_widget_show_all(window);
    int width, height;
    gtk_widget_get_preferred_width(GTK_WIDGET(widget), NULL, &width);
    gtk_widget_get_preferred_height(GTK_WIDGET(widget), NULL, &height);
    GtkAllocation allocation = {0, 0, width, height};
    gtk_widget_size_allocate(GTK_WIDGET(widget), &allocation);
    frame(widget);
    AtkObject *object = gtk_widget_get_accessible(GTK_WIDGET(widget));
    CHECK(ATK_IS_TEXT(object));
    g_object_ref(object);
    AtkText *text = ATK_TEXT(object);
    CHECK(atk_text_get_character_count(text) == 4);
    g_signal_connect(text, "text-remove", G_CALLBACK(removed), widget);
    g_signal_connect(text, "text-insert", G_CALLBACK(inserted), NULL);
    g_signal_connect(text, "text-caret-moved", G_CALLBACK(caret_changed), NULL);
    g_signal_connect(text, "text-selection-changed", G_CALLBACK(selection_changed), NULL);

    feed(terminal, widget, "A日本é🙂\r\nline two");
    CHECK(atk_text_get_character_count(text) == 18);
    CHECK(atk_text_get_caret_offset(text) == 15);
    CHECK(notification_count == 3 && memcmp(notifications, "DIC", 3) == 0);
    char *contents = atk_text_get_text(text, 0, -1);
    CHECK(strcmp(contents, "A日本é🙂\nline two\n\n\n") == 0);
    g_free(contents);
    CHECK(atk_text_get_character_at_offset(text, 1) == 0x65e5);
    CHECK(atk_text_get_character_at_offset(text, 4) == 0x301);
    CHECK(atk_text_get_character_at_offset(text, 5) == 0x1f642);
    int start, end;
    contents = atk_text_get_string_at_offset(text, 9, ATK_TEXT_GRANULARITY_WORD, &start, &end);
    CHECK(contents != NULL && strcmp(contents, "line ") == 0 && start == 7 && end == 12);
    g_free(contents);
    contents = atk_text_get_string_at_offset(text, 4, ATK_TEXT_GRANULARITY_LINE, &start, &end);
    CHECK(contents != NULL && strcmp(contents, "A日本é🙂\n") == 0 && start == 0 && end == 7);
    g_free(contents);
    int x, y, cell_width, cell_height;
    atk_text_get_character_extents(text, 0, &x, &y, &cell_width, &cell_height, ATK_XY_WINDOW);
    CHECK(cell_width > 0 && cell_height > 0);
    int wide_x, wide_y, wide_width, wide_height;
    atk_text_get_character_extents(text, 1, &wide_x, &wide_y, &wide_width, &wide_height, ATK_XY_WINDOW);
    CHECK(wide_x == x + cell_width && wide_y == y && wide_width == 2 * cell_width);
    CHECK(wide_height == cell_height);
    CHECK(atk_text_get_offset_at_point(text, wide_x + cell_width, wide_y, ATK_XY_WINDOW) == 1);
    CHECK(!atk_text_set_caret_offset(text, 0));

    boxrig_ui_view *view = boxrig_gtk3_terminal_widget_view(widget);
    boxrig_ui_hit_test_result anchor, focus;
    CHECK(boxrig_ui_view_hit_test(view, cell_width * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 5 * cell_width * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    notification_count = 0;
    frame(widget);
    CHECK(notification_count == 1 && notifications[0] == 'S');
    CHECK(atk_text_get_n_selections(text) == 1);
    contents = atk_text_get_selection(text, 0, &start, &end);
    CHECK(start == 1 && end == 5 && strcmp(contents, "日本é") == 0);
    g_free(contents);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    frame(widget);
    notification_count = 0;
    feed(terminal, widget, "\rX\033[K");
    CHECK(notification_count == 3 && memcmp(notifications, "DIC", 3) == 0);
    CHECK(atk_text_get_character_count(text) == 11);
    CHECK(atk_text_get_caret_offset(text) == 8);
    notification_count = 0;
    for (int i = 0; i < 4; i++) frame(widget);
    CHECK(notification_count == 0);

    boxrig_composition *composition = boxrig_gtk3_terminal_widget_composition(widget);
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
    CHECK(notification_count == 0);
    contents = atk_text_get_text(text, 0, -1);
    CHECK(strcmp(contents, "A日本é🙂\nX\n\n\n") == 0);
    g_free(contents);
    preedit = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_CANCEL};
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);

    feed(terminal, widget, "\033[?2026h\rY");
    CHECK(notification_count == 0);
    contents = atk_text_get_text(text, 7, 8);
    CHECK(strcmp(contents, "X") == 0);
    g_free(contents);
    feed(terminal, widget, "\033[?2026l");
    CHECK(notification_count == 2 && memcmp(notifications, "DI", 2) == 0);

    feed(terminal, widget, "\033[H\033[1;3;4;31mA");
    AtkAttributeSet *attributes = atk_text_get_run_attributes(text, 0, &start, &end);
    bool bold = false, italic = false, underline = false;
    for (GSList *node = attributes; node != NULL; node = node->next) {
        AtkAttribute *attribute = node->data;
        if (strcmp(attribute->name, "weight") == 0) bold = strcmp(attribute->value, "700") == 0;
        if (strcmp(attribute->name, "style") == 0) italic = strcmp(attribute->value, "italic") == 0;
        if (strcmp(attribute->name, "underline") == 0) underline = strcmp(attribute->value, "single") == 0;
    }
    CHECK(bold && italic && underline && start == 0 && end == 1);
    atk_attribute_set_free(attributes);
    notification_count = 0;
    destroy_on_remove = GTK_WIDGET(widget);
    feed(terminal, widget, "\033[Hreplacement");
    CHECK(notification_count == 1 && notifications[0] == 'D');
    CHECK(atk_text_get_character_count(text) == 0);
    contents = atk_text_get_text(text, 0, -1);
    CHECK(contents != NULL && contents[0] == '\0');
    g_free(contents);
    gtk_widget_destroy(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(atk_text_get_character_count(text) == 0);
    g_object_unref(object);
    g_object_unref(window);
    puts("PASS GTK3 accessible text, notification snapshots and disposal");
    return 0;
}
