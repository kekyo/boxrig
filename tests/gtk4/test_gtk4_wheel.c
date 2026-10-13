/* boxrig - Under MIT. */
#include "boxrig/gtk4.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct wheel_observer {
    size_t count;
    boxrig_mouse_event events[16];
} wheel_observer;

static boxrig_input_mouse_route observe_wheel(const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit, void *context)
{
    wheel_observer *observer = context;
    assert(hit->inside_grid && observer->count < 16);
    observer->events[observer->count++] = *event;
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}

int main(void)
{
    assert(gtk_init_check());
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk4TerminalWidget *widget = NULL;
    boxrig_gtk4_terminal_widget_config wc = boxrig_gtk4_terminal_widget_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_gtk4_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWidget *window = gtk_window_new();
    g_object_ref_sink(window);
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(widget));
    assert(boxrig_gtk4_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
    boxrig_ui_layout_result layout;
    while (boxrig_gtk4_terminal_widget_get_layout(widget, &layout) != BOXRIG_UI_OK)
        (void)g_main_context_iteration(NULL, true);
    boxrig_ui_component_frame_result frame;
    assert(boxrig_gtk4_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    wheel_observer observer = {0};
    assert(boxrig_gtk4_terminal_widget_set_mouse_router(widget, observe_wheel, &observer) == BOXRIG_UI_OK);
    /* Test-only observation; production does not retain the controller model. */
    GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(widget));
    GtkEventControllerScroll *scroll = NULL;
    GtkEventControllerMotion *motion = NULL;
    for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        GObject *item = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_SCROLL(item))
            scroll = GTK_EVENT_CONTROLLER_SCROLL(g_object_ref(item));
        if (GTK_IS_EVENT_CONTROLLER_MOTION(item))
            motion = GTK_EVENT_CONTROLLER_MOTION(g_object_ref(item));
        g_object_unref(item);
    }
    g_object_unref(controllers);
    assert(scroll != NULL && motion != NULL);
    g_signal_emit_by_name(motion, "enter", 1.0, 1.0);
    gboolean handled = FALSE;
    g_signal_emit_by_name(scroll, "scroll", 0.0, -3.0, &handled);
    assert(handled);
    assert(boxrig_gtk4_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 3);
    for (size_t i = 0; i < 3; i++)
        assert(observer.events[i].button == BOXRIG_MOUSE_BUTTON_WHEEL_UP);
    g_signal_emit_by_name(scroll, "scroll", -2.0, 1.0, &handled);
    assert(handled);
    assert(boxrig_gtk4_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 6);
    assert(observer.events[3].button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN);
    assert(observer.events[4].button == BOXRIG_MOUSE_BUTTON_WHEEL_LEFT);
    assert(observer.events[5].button == BOXRIG_MOUSE_BUTTON_WHEEL_LEFT);
    g_signal_emit_by_name(scroll, "scroll", 0.0, 0.0, &handled);
    assert(!handled);
    g_signal_emit_by_name(scroll, "scroll", 0.0, INFINITY, &handled);
    assert(!handled);
    assert(boxrig_gtk4_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 6);
    assert(boxrig_gtk4_terminal_widget_set_mouse_router(widget, NULL, NULL) == BOXRIG_UI_OK);
    const char mode[] = "\033[?1000;1016h";
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)mode, sizeof(mode) - 1,
        &consumed) == BOXRIG_STATUS_OK);
    g_signal_emit_by_name(motion, "enter", 2.25, 3.5);
    g_signal_emit_by_name(scroll, "scroll", 0.0, -1.0, &handled);
    assert(boxrig_gtk4_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    unsigned int scale = (unsigned int)gtk_widget_get_scale_factor(GTK_WIDGET(widget));
    char report[64];
    int length = snprintf(report, sizeof(report), "\033[<64;%u;%uM",
        (unsigned int)(2.25 * scale) + 1, (unsigned int)(3.5 * scale) + 1);
    const uint8_t *pixels;
    assert(length > 0 && boxrig_terminal_output_peek(terminal, &pixels) == (size_t)length);
    assert(memcmp(pixels, report, (size_t)length) == 0);
    assert(boxrig_terminal_output_consume(terminal, (size_t)length) == BOXRIG_STATUS_OK);
    assert(boxrig_gtk4_terminal_widget_set_mouse_router(widget, observe_wheel, &observer) == BOXRIG_UI_OK);
    g_signal_emit_by_name(scroll, "scroll", 0.0,
        (double)wc.maximum_native_input_events + 1.0, &handled);
    assert(!handled);
    boxrig_ui_component_diagnostic diagnostic;
    assert(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
    assert(diagnostic.failed &&
        diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_NATIVE_INPUT_OVERFLOW);
    assert(diagnostic.native_input_queue_depth == 0);
    const uint8_t *output;
    assert(boxrig_terminal_output_peek(terminal, &output) == 0);
    g_object_unref(scroll);
    g_object_unref(motion);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS GTK4 normalized wheel magnitudes, both axes and invalid input");
    return 0;
}
