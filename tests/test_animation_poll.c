/* boxrig - Under MIT. */
#include "boxrig/ui/animation_poll.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static boxrig_ui_status read_time(void *context, uint64_t *time_out)
{
    *time_out = *(uint64_t *)context;
    return BOXRIG_UI_OK;
}

int main(void)
{
    uint64_t now = 100;
    boxrig_ui_animation_poll *adapter = NULL;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    int timeout = 123;
    assert(boxrig_ui_animation_poll_create(read_time, &now, &adapter) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == -1);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -2, &timeout) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(timeout == -1);
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(
        boxrig_ui_animation_poll_runtime(adapter), view) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == 500);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, 30, &timeout) == BOXRIG_UI_OK);
    assert(timeout == 30);
    now = 599;
    assert(boxrig_ui_animation_poll_dispatch(adapter) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == 1);
    now = 600;
    assert(boxrig_ui_animation_poll_dispatch(adapter) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(view));
    now = 1000001100;
    assert(boxrig_ui_animation_poll_dispatch(adapter) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    boxrig_ui_blink_config blink = boxrig_ui_blink_config_default();
    blink.cursor.visible_milliseconds = (uint64_t)INT_MAX + 1000;
    assert(boxrig_ui_view_set_blink_config(view, &blink) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == INT_MAX);
    assert(boxrig_ui_view_set_visible(view, false) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == -1);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    boxrig_ui_animation_poll_destroy(adapter);
    puts("PASS poll adapter deadlines, idle waits and delayed dispatch");
    return 0;
}
