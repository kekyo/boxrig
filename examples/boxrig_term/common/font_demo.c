/* boxrig - Under MIT. */
#include "font_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int boxrig_term_font_demo_apply(boxrig_term_font_demo *demo)
{
    if (!demo->pending) return 0;
    boxrig_ui_status status = demo->setter(demo->context, demo->color_glyphs);
    if (status == BOXRIG_UI_BUSY) return 0;
    if (status != BOXRIG_UI_OK) return -1;
    demo->pending = false;
    fprintf(stderr, "boxrig_term: color-glyphs=%d\n", demo->color_glyphs);
    return 0;
}

int boxrig_term_font_demo_initialize(boxrig_term_font_demo *demo,
    boxrig_ui_view *view, boxrig_term_font_demo_setter setter, void *context)
{
    *demo = (boxrig_term_font_demo){view, setter, context, true, false, false};
    const char *value = getenv("BOXRIG_COLOR_GLYPHS");
    if (value == NULL) return 0;
    if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
        fprintf(stderr, "boxrig_term: BOXRIG_COLOR_GLYPHS must be 0 or 1\n");
        return -1;
    }
    demo->color_glyphs = strcmp(value, "1") == 0;
    demo->enabled = true;
    demo->pending = true;
    return boxrig_term_font_demo_apply(demo);
}

boxrig_input_key_route boxrig_term_font_demo_route_key(
    boxrig_term_font_demo *demo, const boxrig_key_event *event)
{
    if (!demo->enabled || event->key != BOXRIG_KEY_F11 ||
        (event->modifiers & ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK)) !=
            (BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT))
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (event->action == BOXRIG_KEY_PRESS) {
        demo->color_glyphs = !demo->color_glyphs;
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
