/* boxrig - Under MIT. */
#include "boxrig/ui/animation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct scheduler_state {
    uint64_t now;
    bool fail;
    unsigned reservations;
    unsigned cancellations;
} scheduler_state;

static boxrig_ui_status read_time(void *context, uint64_t *now)
{
    *now = ((scheduler_state *)context)->now;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status reserve(void *context, uint64_t deadline, uint64_t generation)
{
    scheduler_state *state = context;
    assert(deadline > state->now && generation != 0);
    if (state->fail) return BOXRIG_UI_UNAVAILABLE;
    state->reservations++;
    return BOXRIG_UI_OK;
}

static void cancel(void *context)
{
    ((scheduler_state *)context)->cancellations++;
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(text));
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *other = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    feed(terminal, "A\033[2;2H\033[6 q");
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &other) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(other, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_update_monotonic_time(other, 0) == BOXRIG_UI_OK);

    boxrig_rgb original_color;
    assert(boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_CURSOR,
        &original_color) == BOXRIG_STATUS_OK);
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default(), actual;
    assert(boxrig_ui_view_get_cursor_style(view, &actual) == BOXRIG_UI_OK);
    assert(actual.shape == BOXRIG_UI_CURSOR_SHAPE_TERMINAL);
    assert(actual.color_policy == BOXRIG_UI_CURSOR_COLOR_TERMINAL);
    assert(actual.blink == BOXRIG_UI_CURSOR_BLINK_TERMINAL && actual.line_width_dip == 1);
    assert(actual.block_presentation == BOXRIG_UI_CURSOR_BLOCK_OUTLINE);
    style.block_presentation = BOXRIG_UI_CURSOR_BLOCK_FILL;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_get_cursor_style(view, &actual) == BOXRIG_UI_OK);
    assert(actual.block_presentation == BOXRIG_UI_CURSOR_BLOCK_FILL);
    boxrig_ui_cursor_style invalid_block = style;
    invalid_block.block_presentation = (boxrig_ui_cursor_block_presentation)2;
    assert(boxrig_ui_view_set_cursor_style(view, &invalid_block) == BOXRIG_UI_INVALID_ARGUMENT);
    style.shape = BOXRIG_UI_CURSOR_SHAPE_OUTLINE;
    style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
    style.color = (boxrig_rgb){23, 45, 67};
    style.blink = BOXRIG_UI_CURSOR_BLINK_ENABLED;
    style.line_width_dip = 2.5f;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_get_cursor_style(view, &actual) == BOXRIG_UI_OK);
    assert(actual.shape == style.shape && actual.color_policy == style.color_policy);
    assert(actual.color.red == 23 && actual.color.green == 45 && actual.color.blue == 67);
    assert(actual.line_width_dip == 2.5f && actual.blink == BOXRIG_UI_CURSOR_BLINK_ENABLED);
    assert(boxrig_ui_view_get_cursor_style(other, &actual) == BOXRIG_UI_OK);
    assert(actual.shape == BOXRIG_UI_CURSOR_SHAPE_TERMINAL);
    uint64_t deadline;
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 500);
    assert(!boxrig_ui_view_next_animation_deadline(other, &deadline));
    assert(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(view));
    assert(boxrig_ui_view_cursor_blink_visible(other));

    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    assert(cursor.shape == BOXRIG_CURSOR_BAR && !cursor.blinking);
    boxrig_rgb color;
    assert(boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_CURSOR,
        &color) == BOXRIG_STATUS_OK);
    assert(memcmp(&color, &original_color, sizeof(color)) == 0);

    feed(terminal, "\033P$q q\033\\");
    const uint8_t *reply;
    const char expected_reply[] = "\033P1$r6 q\033\\";
    assert(boxrig_terminal_output_peek(terminal, &reply) == sizeof(expected_reply) - 1);
    assert(memcmp(reply, expected_reply, sizeof(expected_reply) - 1) == 0);
    assert(boxrig_terminal_output_consume(terminal, sizeof(expected_reply) - 1) == BOXRIG_STATUS_OK);

    style.blink = BOXRIG_UI_CURSOR_BLINK_STEADY;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    for (size_t i = 0; i < 3; i++) {
        boxrig_ui_cursor_style invalid = style;
        invalid.line_width_dip = (float[]){0, NAN, INFINITY}[i];
        assert(boxrig_ui_view_set_cursor_style(view, &invalid) == BOXRIG_UI_INVALID_ARGUMENT);
        assert(boxrig_ui_view_get_cursor_style(view, &actual) == BOXRIG_UI_OK);
        assert(actual.line_width_dip == 2.5f && actual.blink == BOXRIG_UI_CURSOR_BLINK_STEADY);
    }
    assert(boxrig_ui_view_reset_cursor_style(view) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_get_cursor_style(view, &actual) == BOXRIG_UI_OK);
    assert(actual.shape == BOXRIG_UI_CURSOR_SHAPE_TERMINAL && actual.line_width_dip == 1);
    assert(actual.block_presentation == BOXRIG_UI_CURSOR_BLOCK_OUTLINE);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));

    scheduler_state state = {1000, false, 0, 0};
    boxrig_ui_animation_scheduler scheduler = {&state, read_time, reserve, cancel};
    boxrig_ui_animation_runtime *runtime = NULL;
    assert(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_runtime_attach(runtime, view) == BOXRIG_UI_OK);
    style.blink = BOXRIG_UI_CURSOR_BLINK_ENABLED;
    state.fail = true;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_ui_view_get_cursor_style(view, &actual) == BOXRIG_UI_OK);
    assert(actual.blink == BOXRIG_UI_CURSOR_BLINK_TERMINAL);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    state.fail = false;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(state.reservations == 1);
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 1500);
    feed(terminal, "\033[?25l");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    assert(state.cancellations == 1);
    boxrig_ui_animation_runtime_detach(view);
    boxrig_ui_animation_runtime_destroy(runtime);
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS view cursor style, terminal isolation, blinking precedence and scheduling rollback");
    return 0;
}
