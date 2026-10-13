/* boxrig - Under MIT. */
#include "scrollbar_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int boxrig_term_scrollbar_demo_apply(boxrig_term_scrollbar_demo *demo)
{
    if (!demo->pending) return 0;
    boxrig_ui_status status = demo->setter(demo->context, demo->policy);
    if (status == BOXRIG_UI_BUSY) return 0;
    if (status != BOXRIG_UI_OK) return -1;
    demo->pending = false;
    const char *names[] = {"always", "auto", "never"};
    fprintf(stderr, "boxrig_term: scrollbar=%s\n", names[demo->policy]);
    return 0;
}

int boxrig_term_scrollbar_demo_initialize(boxrig_term_scrollbar_demo *demo,
    boxrig_ui_view *view, boxrig_term_scrollbar_demo_setter setter, void *context)
{
    *demo = (boxrig_term_scrollbar_demo){view, setter, context,
        BOXRIG_UI_SCROLLBAR_ALWAYS, false, false};
    const char *value = getenv("BOXRIG_SCROLLBAR");
    if (value == NULL) return 0;
    if (strcmp(value, "always") == 0) demo->policy = BOXRIG_UI_SCROLLBAR_ALWAYS;
    else if (strcmp(value, "auto") == 0) demo->policy = BOXRIG_UI_SCROLLBAR_AUTO;
    else if (strcmp(value, "never") == 0) demo->policy = BOXRIG_UI_SCROLLBAR_NEVER;
    else {
        fprintf(stderr, "boxrig_term: BOXRIG_SCROLLBAR must be always, auto or never\n");
        return -1;
    }
    demo->enabled = true;
    demo->pending = true;
    return boxrig_term_scrollbar_demo_apply(demo);
}

boxrig_input_key_route boxrig_term_scrollbar_demo_route_key(
    boxrig_term_scrollbar_demo *demo, const boxrig_key_event *event)
{
    if (!demo->enabled || event->key != BOXRIG_KEY_F9 ||
        (event->modifiers & ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK)) !=
            (BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT))
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (event->action == BOXRIG_KEY_PRESS) {
        demo->policy = demo->policy == BOXRIG_UI_SCROLLBAR_NEVER
            ? BOXRIG_UI_SCROLLBAR_ALWAYS : demo->policy + 1;
        demo->pending = true;
        boxrig_ui_dirty dirty = {0};
        boxrig_terminal_size(boxrig_ui_model_terminal(boxrig_ui_view_model(demo->view)),
            &dirty.row_count, &dirty.column_count);
        if (boxrig_ui_view_invalidate(demo->view, BOXRIG_UI_DIRTY_PRESENT,
                dirty) != BOXRIG_UI_OK)
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}
