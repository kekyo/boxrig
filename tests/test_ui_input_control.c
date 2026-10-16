#include "../src/ui/input_control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void output_is(boxrig_terminal *terminal, const char *expected)
{
    const uint8_t *bytes;
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);
    assert(length == strlen(expected));
    assert(memcmp(bytes, expected, length) == 0);
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.output_capacity = 6;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_ui_input_control control;
    boxrig_ui_input_control_initialize(&control);
    uint64_t before = control.state.generation;
    assert(control.state.phase == BOXRIG_UI_INPUT_ENABLED);
    assert(boxrig_ui_input_control_accepts(&control, before));
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[?2004h", 8,
                               &consumed) == BOXRIG_STATUS_OK);
    boxrig_paste_event event = {BOXRIG_PASTE_BEGIN, NULL, 0};
    assert(boxrig_terminal_send_paste(terminal, &event) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_input_control_request(&control, false, true) == BOXRIG_UI_BUSY);
    uint64_t stopped = control.state.generation;
    assert(stopped > before && control.state.phase == BOXRIG_UI_INPUT_STOPPING);
    assert(!boxrig_ui_input_control_accepts(&control, before));
    assert(boxrig_ui_input_control_service(&control, terminal) == BOXRIG_UI_BUSY);
    assert(boxrig_ui_input_control_request(&control, true, false) == BOXRIG_UI_BUSY);
    assert(boxrig_ui_input_control_request(&control, false, false) == BOXRIG_UI_BUSY);
    assert(control.state.generation == stopped);
    output_is(terminal, "\x1b[200~");

    /* Terminal replies keep working while the paste terminator needs capacity. */
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[5n", 4,
                               &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_input_control_service(&control, terminal) == BOXRIG_UI_BUSY);
    output_is(terminal, "\x1b[0n");
    assert(boxrig_ui_input_control_service(&control, terminal) == BOXRIG_UI_OK);
    assert(control.state.phase == BOXRIG_UI_INPUT_DISABLED);
    /* Completion means queued in core, not drained to the communication channel. */
    output_is(terminal, "\x1b[201~");
    assert(boxrig_ui_input_control_request(&control, true, false) == BOXRIG_UI_OK);
    assert(control.state.phase == BOXRIG_UI_INPUT_ENABLED);
    assert(control.state.generation > stopped);
    assert(!boxrig_ui_input_control_accepts(&control, stopped));
    assert(boxrig_ui_input_control_accepts(&control, control.state.generation));
    assert(boxrig_ui_input_control_service(&control, terminal) == BOXRIG_UI_OK);
    const uint8_t *bytes;
    assert(boxrig_terminal_output_peek(terminal, &bytes) == 0);

    /* An unstarted paste needs no closing bytes. Repeated requests are harmless. */
    assert(boxrig_ui_input_control_request(&control, false, false) == BOXRIG_UI_OK);
    uint64_t disabled = control.state.generation;
    assert(control.state.phase == BOXRIG_UI_INPUT_DISABLED);
    assert(boxrig_ui_input_control_request(&control, false, false) == BOXRIG_UI_OK);
    assert(control.state.generation == disabled);
    assert(boxrig_terminal_output_peek(terminal, &bytes) == 0);
    control.state.generation = UINT64_MAX;
    assert(boxrig_ui_input_control_request(&control, true, false) == BOXRIG_UI_LIMIT_EXCEEDED);
    assert(control.state.phase == BOXRIG_UI_INPUT_DISABLED);
    boxrig_terminal_destroy(terminal);
    puts("PASS input generations, paste stop completion, and continued terminal replies");
    return 0;
}
