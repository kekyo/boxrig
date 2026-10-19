/* boxrig - Under MIT. */
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3.h"
#define Widget BoxrigGtk3TerminalWidget
#define config_default boxrig_gtk3_terminal_widget_config_default
#define widget_create boxrig_gtk3_terminal_widget_create
#define widget_quiesce boxrig_gtk3_terminal_widget_quiesce
#define widget_set_font boxrig_gtk3_terminal_widget_set_font_specification
#define widget_get_font boxrig_gtk3_terminal_widget_get_font_specification
#define widget_metrics boxrig_gtk3_terminal_widget_get_effective_metrics
#else
#include "boxrig/gtk4.h"
#define Widget BoxrigGtk4TerminalWidget
#define config_default boxrig_gtk4_terminal_widget_config_default
#define widget_create boxrig_gtk4_terminal_widget_create
#define widget_quiesce boxrig_gtk4_terminal_widget_quiesce
#define widget_set_font boxrig_gtk4_terminal_widget_set_font_specification
#define widget_get_font boxrig_gtk4_terminal_widget_get_font_specification
#define widget_metrics boxrig_gtk4_terminal_widget_get_effective_metrics
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    Widget *first = NULL, *second = NULL;
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
#ifdef BOXRIG_TEST_GTK3
    boxrig_gtk3_terminal_widget_config config = config_default();
#else
    boxrig_gtk4_terminal_widget_config config = config_default();
#endif
    assert(config.initial_font == NULL);
    assert(widget_create(model, &config, &first) == BOXRIG_UI_OK);
    g_object_ref_sink(first);
    boxrig_font_specification font = boxrig_font_specification_default();
    char family_name[] = "Monospace";
    boxrig_font_family family = {family_name, strlen(family_name), BOXRIG_FONT_RESOURCE_NONE};
    font.families = &family;
    font.family_count = 1;
    font.size_dip = 19;
    assert(widget_set_font(first, &font) == BOXRIG_UI_OK);
    boxrig_font_effective_metrics before, after;
    assert(widget_metrics(first, &before) == BOXRIG_FONT_CATALOG_OK);
    assert(widget_quiesce(first) == BOXRIG_UI_OK);
    config.initial_font = &font;
    assert(widget_create(model, &config, &second) == BOXRIG_UI_OK);
    g_object_ref_sink(second);
    family_name[0] = 'X';
    font.size_dip = 27;
    boxrig_font_specification copied;
    assert(widget_get_font(second, &copied) == BOXRIG_UI_OK);
    assert(copied.size_dip == 19 && copied.family_count == 1);
    assert(memcmp(copied.families[0].name, "Monospace", 9) == 0);
    assert(widget_metrics(second, &after) == BOXRIG_FONT_CATALOG_OK);
    assert(before.cell_width == after.cell_width && before.cell_height == after.cell_height);
    assert(widget_quiesce(second) == BOXRIG_UI_OK);
    g_object_unref(second);
    g_object_unref(first);
    font.size_dip = -1;
    second = NULL;
    assert(widget_create(model, &config, &second) == BOXRIG_UI_INVALID_ARGUMENT && second == NULL);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS initial font metrics and owned font data through widget replacement");
    return 0;
}
