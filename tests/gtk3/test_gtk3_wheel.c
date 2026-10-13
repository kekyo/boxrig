/* boxrig - Under MIT. */
#include "boxrig/gtk3.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

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

int main(int argc, char **argv)
{
    assert(gtk_init_check(&argc, &argv));
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    BoxrigGtk3TerminalWidget *widget = NULL;
    boxrig_gtk3_terminal_widget_config wc = boxrig_gtk3_terminal_widget_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_gtk3_terminal_widget_create(model, &wc, &widget) == BOXRIG_UI_OK);
    g_object_ref_sink(widget);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(window);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(widget));
    assert(boxrig_gtk3_terminal_widget_set_visible(widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(window);
    boxrig_ui_layout_result layout;
    while (boxrig_gtk3_terminal_widget_get_layout(widget, &layout) != BOXRIG_UI_OK)
        (void)g_main_context_iteration(NULL, true);
    boxrig_ui_component_frame_result frame;
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    wheel_observer observer = {0};
    assert(boxrig_gtk3_terminal_widget_set_mouse_router(widget, observe_wheel, &observer) == BOXRIG_UI_OK);
    GdkEventScroll event = {0};
    event.type = GDK_SCROLL;
    event.direction = GDK_SCROLL_SMOOTH;
    event.x = event.y = 1.0;
    event.delta_y = -0.25;
    for (size_t i = 0; i < 4; i++) {
        gboolean handled = FALSE;
        g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
        assert(handled);
        assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
        assert(observer.count == (i == 3 ? 1 : 0));
    }
    assert(observer.events[0].button == BOXRIG_MOUSE_BUTTON_WHEEL_UP);
    event.delta_y = 2.75;
    gboolean handled = FALSE;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(handled);
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 3);
    event.delta_y = 0.25;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 4);
    for (size_t i = 1; i < 4; i++)
        assert(observer.events[i].button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN);
    event.delta_x = -2.0;
    event.delta_y = -1.0;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 7);
    assert(observer.events[4].button == BOXRIG_MOUSE_BUTTON_WHEEL_UP);
    assert(observer.events[5].button == BOXRIG_MOUSE_BUTTON_WHEEL_LEFT);
    assert(observer.events[6].button == BOXRIG_MOUSE_BUTTON_WHEEL_LEFT);
    /* Fractions belong to the current modifier gesture. */
    event.delta_x = 0;
    event.delta_y = 0.75;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    event.state = GDK_CONTROL_MASK;
    event.delta_y = 0.25;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 7);
    event.delta_y = 0.75;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 8 && observer.events[7].modifiers == BOXRIG_MODIFIER_CONTROL);
    event.delta_y = 0.75;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    event.is_stop = 1;
    event.delta_y = 0;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    event.is_stop = 0;
    event.delta_y = 0.25;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(boxrig_gtk3_terminal_widget_frame(widget, &frame) == BOXRIG_UI_OK);
    assert(observer.count == 8);
    event.delta_y = INFINITY;
    handled = TRUE;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(!handled);
    event.delta_y = (double)wc.maximum_native_input_events + 1.0;
    g_signal_emit_by_name(widget, "scroll-event", &event, &handled);
    assert(!handled);
    boxrig_ui_component_diagnostic diagnostic;
    assert(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) == BOXRIG_UI_OK);
    assert(diagnostic.failed &&
        diagnostic.failure_reason == BOXRIG_UI_COMPONENT_FAILURE_NATIVE_INPUT_OVERFLOW);
    assert(diagnostic.native_input_queue_depth == 0);
    const uint8_t *output;
    assert(boxrig_terminal_output_peek(terminal, &output) == 0);
    gtk_widget_destroy(window);
    g_object_unref(window);
    g_object_unref(widget);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS GTK3 smooth wheel accumulation, axes, modifiers and stop");
    return 0;
}
