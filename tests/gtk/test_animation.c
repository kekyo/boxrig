/* boxrig - Under MIT. */
#include "animation.h"
#include <assert.h>
#include <stdio.h>

static gint64 now;
gint64 __wrap_g_get_monotonic_time(void)
{
    return now * 1000;
}

int main(void)
{
    GMainContext *context = g_main_context_new();
    GMainContext *other_context = g_main_context_new();
    boxrig_gtk_animation *first = NULL, *second = NULL, *other = NULL;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *a = NULL, *b = NULL;
    tc.rows = 2;
    tc.columns = 8;
    assert(boxrig_gtk_animation_acquire(context, &first) == BOXRIG_UI_OK);
    assert(boxrig_gtk_animation_acquire(context, &second) == BOXRIG_UI_OK);
    assert(first == second);
    assert(boxrig_gtk_animation_acquire(other_context, &other) == BOXRIG_UI_OK);
    assert(!g_main_context_pending(context));
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &a) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &b) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(a, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(b, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(boxrig_gtk_animation_runtime(first), a) ==
        BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(boxrig_gtk_animation_runtime(other), b) ==
        BOXRIG_UI_BUSY);
    assert(boxrig_ui_animation_runtime_attach(boxrig_gtk_animation_runtime(second), b) ==
        BOXRIG_UI_OK);
    assert(!g_main_context_iteration(other_context, FALSE));
    assert(boxrig_ui_view_cursor_blink_visible(a));
    assert(boxrig_ui_view_cursor_blink_visible(b));
    now = 500;
    assert(g_main_context_iteration(context, FALSE));
    assert(!boxrig_ui_view_cursor_blink_visible(a));
    assert(!boxrig_ui_view_cursor_blink_visible(b));
    assert(boxrig_ui_view_set_visible(b, false) == BOXRIG_UI_OK);
    boxrig_ui_blink_config invalid = vc.blink;
    invalid.cursor.visible_milliseconds = (uint64_t)G_MAXINT64;
    assert(boxrig_ui_view_set_blink_config(a, &invalid) == BOXRIG_UI_LIMIT_EXCEEDED);
    boxrig_ui_blink_config unchanged;
    assert(boxrig_ui_view_get_blink_config(a, &unchanged) == BOXRIG_UI_OK);
    assert(unchanged.cursor.visible_milliseconds == 500);
    boxrig_ui_view_destroy(a);
    boxrig_gtk_animation_release(first);
    assert(boxrig_ui_view_set_visible(b, true) == BOXRIG_UI_OK);
    now = 1500;
    assert(g_main_context_iteration(context, FALSE));
    assert(boxrig_ui_view_cursor_blink_visible(b));
    assert(boxrig_ui_view_set_visible(b, false) == BOXRIG_UI_OK);
    assert(!g_main_context_pending(context));
    assert(boxrig_ui_view_set_visible(b, true) == BOXRIG_UI_OK);
    assert(g_main_context_pending(context));
    boxrig_ui_view_destroy(b);
    boxrig_gtk_animation_release(second);
    assert(!g_main_context_iteration(context, FALSE));
    boxrig_gtk_animation_release(other);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    g_main_context_unref(context);
    g_main_context_unref(other_context);
    puts("PASS GTK context clocks, shared ownership, overflow rollback and cancellation");
    return 0;
}
