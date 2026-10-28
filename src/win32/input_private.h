/* boxrig - Under MIT. */
#ifndef BOXRIG_WIN32_INPUT_PRIVATE_H
#define BOXRIG_WIN32_INPUT_PRIVATE_H

#include "boxrig/win32/input.h"

/* The component owns this borrowed view and disconnects it on quiesce. */
void boxrig_win32_input_adapter_set_animation_view(
    boxrig_win32_input_adapter *adapter, boxrig_ui_view *view);

/* Native callbacks retain failures because Windows cannot return a UI status. */
void boxrig_win32_input_adapter_report_animation_failure(
    boxrig_win32_input_adapter *adapter, boxrig_ui_status status);
boxrig_ui_status boxrig_win32_input_adapter_animation_status(
    const boxrig_win32_input_adapter *adapter);

/* Validates a native parent move without changing the retained input state. */
boxrig_ui_status boxrig_win32_input_adapter_check_reparent(
    const boxrig_win32_input_adapter *adapter);

/* Invalidates native input queued before a successful parent move. */
void boxrig_win32_input_adapter_complete_reparent(
    boxrig_win32_input_adapter *adapter);

#endif
