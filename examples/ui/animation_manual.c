/* Lower-level animation integration for a custom renderer - Under MIT. */
#include "boxrig/ui.h"

#include <inttypes.h>
#include <stdio.h>

static bool next_deadline(boxrig_terminal *terminal, boxrig_ui_view *view,
    uint64_t *deadline_out)
{
    uint64_t view_deadline = 0;
    bool found = boxrig_terminal_next_animation_deadline(terminal, deadline_out);
    if (boxrig_ui_view_next_animation_deadline(view, &view_deadline) &&
        (!found || view_deadline < *deadline_out)) {
        *deadline_out = view_deadline;
        found = true;
    }
    return found;
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    int result = 1;
    config.rows = 1;
    config.columns = 8;
    const char text[] = "\033[5mS\033[6mR\033[0m\033[1 q";
    size_t consumed = 0;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1,
            &consumed) != BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        boxrig_ui_view_set_focused(view, true) != BOXRIG_UI_OK)
        goto cleanup;
    const uint64_t times[] = {0, 150, 500, 1000};
    const bool slow[] = {true, true, false, true};
    const bool rapid[] = {true, false, false, true};
    for (size_t i = 0; i < sizeof(times) / sizeof(*times); i++) {
        /* One monotonic domain drives terminal images and view decoration.
         * This route is only for a view without an automatic runtime. */
        if (boxrig_terminal_update_monotonic_time(terminal, times[i]) != BOXRIG_STATUS_OK ||
            boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK ||
            boxrig_ui_view_update_monotonic_time(view, times[i]) != BOXRIG_UI_OK)
            goto cleanup;
        bool slow_visible = boxrig_ui_view_text_blink_visible(view, BOXRIG_CELL_BLINK);
        bool rapid_visible = boxrig_ui_view_text_blink_visible(view, BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK);
        bool cursor_visible = boxrig_ui_view_cursor_blink_visible(view);
        if (slow_visible != slow[i] || rapid_visible != rapid[i] ||
            cursor_visible != slow[i])
            goto cleanup;
        /* A custom renderer keeps background/images and suppresses only ink,
         * underline and strike when the corresponding text phase is hidden. */
        uint64_t deadline = 0;
        if (!next_deadline(terminal, view, &deadline) || deadline <= times[i])
            goto cleanup;
        printf("time=%" PRIu64 " slow=%u rapid=%u cursor=%u next=%" PRIu64 "\n",
            times[i], slow_visible ? 1u : 0u, rapid_visible ? 1u : 0u,
            cursor_visible ? 1u : 0u, deadline);
    }
    if (boxrig_ui_view_set_visible(view, false) != BOXRIG_UI_OK) goto cleanup;
    uint64_t deadline = 0;
    if (next_deadline(terminal, view, &deadline)) goto cleanup;
    result = 0;
cleanup:
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (result == 0) puts("PASS manual animation phases and combined deadline");
    return result;
}
