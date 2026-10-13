/* Installed SDK consumer: external binding errors and portable cursor state. */
#include "external_scrollbar.h"
#include <stdio.h>

static void unexpected_notification(void *context, const boxrig_scrollbar_state *state)
{
    (void)state;
    *(bool *)context = true;
}

int main(void)
{
    example_external_scrollbar bar = {0};
    bool notified = false;
    if (example_external_scrollbar_refresh(&bar, unexpected_notification, &notified) !=
            BOXRIG_UI_INVALID_ARGUMENT || notified) return 1;
    bool changed = true;
    if (example_external_scrollbar_action(&bar, BOXRIG_SCROLLBAR_BOTTOM, 0, &changed) !=
            BOXRIG_UI_INVALID_ARGUMENT) return 1;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &vc, &view) != BOXRIG_UI_OK) return 1;
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.block_presentation = BOXRIG_UI_CURSOR_BLOCK_FILL;
    boxrig_scrollbar_state state;
    bool passed = boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK &&
        boxrig_ui_view_get_scrollbar_state(view, &state) == BOXRIG_UI_OK &&
        state.revision != 0 && !state.model.enabled;
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (!passed) return 1;
    puts("PASS installed scrollbar example, public state and filled cursor API");
    return 0;
}
