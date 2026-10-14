/* Shared frontend state; native queues and asynchronous sources belong to the adapter. */
#ifndef BOXRIG_UI_INPUT_CONTROL_PRIVATE_H
#define BOXRIG_UI_INPUT_CONTROL_PRIVATE_H

#include "boxrig/ui/input.h"

typedef struct boxrig_ui_input_control {
    boxrig_ui_input_state state;
    bool paste_end_pending;
} boxrig_ui_input_control;

void boxrig_ui_input_control_initialize(boxrig_ui_input_control *control);

/* The adapter records whether its paste BEGIN reached core, then discards all
 * old user input before exposing completion. A BUSY stop is already accepted. */
boxrig_ui_status boxrig_ui_input_control_request(
    boxrig_ui_input_control *control, bool enabled, bool paste_started);

/* Called from the regular input dispatch/retry path, never from a timer. */
boxrig_ui_status boxrig_ui_input_control_service(
    boxrig_ui_input_control *control, boxrig_terminal *terminal);

bool boxrig_ui_input_control_accepts(
    const boxrig_ui_input_control *control, uint64_t generation);

#endif
