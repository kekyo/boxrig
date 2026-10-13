/* boxrig - Under MIT. */
#include "../examples/boxrig_term/common/scrollbar_demo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct scrollbar_sink {
    boxrig_ui_status status;
    boxrig_ui_scrollbar_visibility accepted;
    unsigned int calls;
} scrollbar_sink;

static boxrig_ui_status apply_policy(void *context, boxrig_ui_scrollbar_visibility policy)
{
    scrollbar_sink *sink = context;
    sink->calls++;
    if (sink->status == BOXRIG_UI_OK) sink->accepted = policy;
    return sink->status;
}

int main(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    boxrig_term_scrollbar_demo demo;
    scrollbar_sink sink = {BOXRIG_UI_OK, BOXRIG_UI_SCROLLBAR_ALWAYS, 0};
    boxrig_key_event key = {0};
    key.key = BOXRIG_KEY_F9;
    key.action = BOXRIG_KEY_PRESS;
    key.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    assert(unsetenv("BOXRIG_SCROLLBAR") == 0);
    assert(boxrig_term_scrollbar_demo_initialize(&demo, view, apply_policy, &sink) == 0);
    assert(sink.calls == 0);
    assert(boxrig_term_scrollbar_demo_route_key(&demo, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    const char *values[] = {"always", "auto", "never"};
    for (unsigned int i = 0; i < 3; i++) {
        assert(setenv("BOXRIG_SCROLLBAR", values[i], 1) == 0);
        assert(boxrig_term_scrollbar_demo_initialize(&demo, view, apply_policy, &sink) == 0);
        assert(sink.accepted == (boxrig_ui_scrollbar_visibility)i);
    }
    unsigned int calls = sink.calls;
    key.modifiers |= BOXRIG_MODIFIER_ALT;
    assert(boxrig_term_scrollbar_demo_route_key(&demo, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    key.modifiers &= ~BOXRIG_MODIFIER_ALT;
    sink.status = BOXRIG_UI_BUSY;
    assert(boxrig_term_scrollbar_demo_route_key(&demo, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(boxrig_term_scrollbar_demo_apply(&demo) == 0);
    assert(sink.accepted == BOXRIG_UI_SCROLLBAR_NEVER);
    /* A second request replaces the first while the native layout is busy. */
    assert(boxrig_term_scrollbar_demo_route_key(&demo, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    key.action = BOXRIG_KEY_RELEASE;
    assert(boxrig_term_scrollbar_demo_route_key(&demo, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    sink.status = BOXRIG_UI_OK;
    assert(boxrig_term_scrollbar_demo_apply(&demo) == 0);
    assert(sink.accepted == BOXRIG_UI_SCROLLBAR_AUTO && sink.calls == calls + 2);
    assert(boxrig_term_scrollbar_demo_apply(&demo) == 0 && sink.calls == calls + 2);
    key.action = BOXRIG_KEY_PRESS;
    assert(boxrig_term_scrollbar_demo_route_key(&demo, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    sink.status = BOXRIG_UI_UNAVAILABLE;
    assert(boxrig_term_scrollbar_demo_apply(&demo) == -1);
    assert(setenv("BOXRIG_SCROLLBAR", "invalid", 1) == 0);
    assert(boxrig_term_scrollbar_demo_initialize(&demo, view, apply_policy, &sink) == -1);
    assert(unsetenv("BOXRIG_SCROLLBAR") == 0);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS scrollbar demo selection, key routing and deferred native updates");
    return 0;
}
