/* boxrig - Under MIT. */
#include "mouse_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK && consumed == strlen(text));
}

static size_t offset(boxrig_terminal *terminal)
{
    boxrig_scrollbar_model model;
    assert(boxrig_scrollbar_model_read(terminal, &model) == 0);
    return model.viewport_offset;
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    feed(terminal, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\nseven\r\neight\r\nnine\r\nten");
    boxrig_scrollbar_model model;
    assert(boxrig_scrollbar_model_read(terminal, &model) == 0);
    assert(model.history_rows == 7 && model.viewport_offset == 0);
    boxrig_mouse_event event = {0};
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    boxrig_ui_hit_test_result hit = {0};
    hit.inside_grid = true;
    boxrig_ui_zoom_shortcut zoom;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE && offset(terminal) == 3);
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(offset(terminal) == 6);
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(offset(terminal) == 7);
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(offset(terminal) == 7);
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_DOWN;
    for (unsigned int i = 0; i < 3; i++)
        assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
            &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(offset(terminal) == 0);

    feed(terminal, "\033[?1003;1006h");
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    assert(offset(terminal) == 0 && zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    assert(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    const uint8_t *output;
    size_t output_length;
    output_length = boxrig_terminal_output_peek(terminal, &output);
    assert(output_length != 0);
    assert(boxrig_terminal_output_consume(terminal, output_length) == BOXRIG_STATUS_OK);
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(offset(terminal) == 3 && zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    for (unsigned int shift = 0; shift < 2; shift++) {
        event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_CAPS_LOCK |
            BOXRIG_MODIFIER_NUM_LOCK | (shift ? BOXRIG_MODIFIER_SHIFT : 0);
        event.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
        assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
            &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
        assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_IN && offset(terminal) == 3);
        event.button = BOXRIG_MOUSE_BUTTON_WHEEL_DOWN;
        assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
            &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
        assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_OUT && offset(terminal) == 3);
    }
    assert(boxrig_term_mouse_policy_route_wheel(terminal, true, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE && offset(terminal) == 3);
    const uint32_t other_modifiers[] = {BOXRIG_MODIFIER_ALT, BOXRIG_MODIFIER_SUPER,
        BOXRIG_MODIFIER_HYPER, BOXRIG_MODIFIER_META};
    for (size_t i = 0; i < sizeof(other_modifiers) / sizeof(*other_modifiers); i++) {
        event.modifiers = BOXRIG_MODIFIER_CONTROL | other_modifiers[i];
        assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
            &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
        assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE && offset(terminal) == 3);
    }
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    hit.inside_grid = false;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    hit.inside_grid = true;
    event.action = BOXRIG_MOUSE_RELEASE;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_LEFT;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    feed(terminal, "\033[?1049h");
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_IN);
    feed(terminal, "\033[?1003l\033[?1007h");
    event.modifiers = 0;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    assert(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    assert(output_length == 3 && memcmp(output, "\033[A", 3) == 0);
    assert(boxrig_terminal_output_consume(terminal, output_length) == BOXRIG_STATUS_OK);
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    assert(zoom == BOXRIG_UI_ZOOM_SHORTCUT_IN);
    feed(terminal, "\033[?1000h");
    event.modifiers = 0;
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    assert(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    assert(output_length == 10 && memcmp(output, "\033[<64;1;1M", 10) == 0);
    assert(boxrig_terminal_output_consume(terminal, output_length) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[?1000l\033[?1007l");
    assert(boxrig_term_mouse_policy_route_wheel(terminal, false, &event,
        &hit, &zoom) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    feed(terminal, "\033[?1049l");
    assert(offset(terminal) == 3);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    assert(output_length == 0);
    boxrig_terminal_destroy(terminal);
    puts("PASS wheel history, reporting exclusion, modifiers and zoom decisions");
    return 0;
}
