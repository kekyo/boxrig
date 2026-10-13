/* boxrig - Under MIT. */
#include "boxrig/ui/animation.h"
#ifdef BOXRIG_TEST_GTK3
#include "boxrig/gtk3/terminal_widget.h"
#define Widget BoxrigGtk3TerminalWidget
#define widget_config boxrig_gtk3_terminal_widget_config
#define config_default boxrig_gtk3_terminal_widget_config_default
#define widget_create boxrig_gtk3_terminal_widget_create
#define widget_view boxrig_gtk3_terminal_widget_view
#define widget_quiesce boxrig_gtk3_terminal_widget_quiesce
#define widget_queue_paste boxrig_gtk3_terminal_widget_queue_paste
#define widget_composition boxrig_gtk3_terminal_widget_composition
#define widget_frame boxrig_gtk3_terminal_widget_frame
#define widget_diagnostic boxrig_gtk3_terminal_widget_get_diagnostic
#define widget_visible boxrig_gtk3_terminal_widget_set_visible
#else
#include "boxrig/gtk4/terminal_widget.h"
#define Widget BoxrigGtk4TerminalWidget
#define widget_config boxrig_gtk4_terminal_widget_config
#define config_default boxrig_gtk4_terminal_widget_config_default
#define widget_create boxrig_gtk4_terminal_widget_create
#define widget_view boxrig_gtk4_terminal_widget_view
#define widget_quiesce boxrig_gtk4_terminal_widget_quiesce
#define widget_queue_paste boxrig_gtk4_terminal_widget_queue_paste
#define widget_composition boxrig_gtk4_terminal_widget_composition
#define widget_frame boxrig_gtk4_terminal_widget_frame
#define widget_diagnostic boxrig_gtk4_terminal_widget_get_diagnostic
#define widget_visible boxrig_gtk4_terminal_widget_set_visible
#endif
#include <assert.h>
#include <stdio.h>

static gint64 now;
gint64 __wrap_g_get_monotonic_time(void)
{
    return now * 1000;
}

int main(void)
{
#ifdef BOXRIG_TEST_GTK3
    gtk_init(NULL, NULL);
#else
    gtk_init();
#endif
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    widget_config config = config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    Widget *first = NULL, *second = NULL;
    tc.rows = 2;
    tc.columns = 8;
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(widget_create(model, &config, &first) == BOXRIG_UI_OK);
    g_object_ref_sink(first);
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_show(GTK_WIDGET(first));
    GtkAllocation allocation = {0, 0, 240, 80};
    gtk_widget_size_allocate(GTK_WIDGET(first), &allocation);
#else
    gtk_widget_allocate(GTK_WIDGET(first), 240, 80, -1, NULL);
#endif
    assert(boxrig_terminal_update_monotonic_time(terminal, 0) == BOXRIG_STATUS_BUSY);
    config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
    assert(widget_create(model, &config, &second) == BOXRIG_UI_OK);
    g_object_ref_sink(second);
    boxrig_ui_view *view = widget_view(first);
    assert(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_visible(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    now = 500;
    assert(g_main_context_iteration(context, FALSE));
    assert(!boxrig_ui_view_cursor_blink_visible(view));
    assert(widget_queue_paste(first, (const uint8_t *)"x", 1) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    now = 1000;
    assert(g_main_context_iteration(context, FALSE));
    assert(!boxrig_ui_view_cursor_blink_visible(view));
    boxrig_composition *composition = widget_composition(first);
    assert(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event event = {0};
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = "a";
    event.text_length = 1;
    assert(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    boxrig_ui_component_frame_result frame;
    assert(widget_frame(first, &frame) == BOXRIG_UI_OK);
    uint64_t deadline;
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    event = (boxrig_composition_event){0};
    event.type = BOXRIG_COMPOSITION_CANCEL;
    assert(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    assert(widget_frame(first, &frame) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 1500);
    now = -1;
    assert(g_main_context_iteration(context, FALSE));
    boxrig_ui_component_diagnostic diagnostic;
    assert(widget_diagnostic(first, &diagnostic) == BOXRIG_UI_OK);
    assert(diagnostic.failed && !diagnostic.running);
    assert(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_ANIMATION);
    assert(widget_frame(first, &frame) == BOXRIG_UI_UNAVAILABLE);
    now = 1000;
    assert(widget_quiesce(first) == BOXRIG_UI_OK);
    assert(boxrig_terminal_update_monotonic_time(terminal, 1000) == BOXRIG_STATUS_BUSY);
    assert(widget_quiesce(second) == BOXRIG_UI_OK);
    assert(boxrig_terminal_update_monotonic_time(terminal, 1000) == BOXRIG_STATUS_OK);
    g_object_unref(first);
    g_object_unref(second);
    config = config_default();
    assert(widget_create(model, &config, &first) == BOXRIG_UI_OK);
    g_object_ref_sink(first);
    assert(widget_visible(first, true) == BOXRIG_UI_OK);
#ifdef BOXRIG_TEST_GTK3
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(first));
    gtk_widget_show(GTK_WIDGET(first));
#else
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(first));
#endif
    gtk_widget_set_visible(window, true);
    gtk_widget_set_visible(window, false);
    assert(!gtk_widget_get_mapped(GTK_WIDGET(first)));
    now = -1;
    gtk_widget_set_visible(window, true);
    assert(widget_diagnostic(first, &diagnostic) == BOXRIG_UI_OK);
    assert(diagnostic.failed && !diagnostic.running);
    assert(diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_ANIMATION);
    now = 1000;
    assert(widget_quiesce(first) == BOXRIG_UI_OK);
#ifdef BOXRIG_TEST_GTK3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    unsigned pending = 0;
    while (g_main_context_iteration(context, FALSE)) assert(++pending < 1000);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    puts("PASS GTK widget context ownership, cursor phase and quiesce");
    return 0;
}
