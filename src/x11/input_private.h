#ifndef BOXRIG_X11_INPUT_PRIVATE_H
#define BOXRIG_X11_INPUT_PRIVATE_H

#include "boxrig/x11/input.h"

/* Rebinds the input method's one-shot client window while retaining the adapter. */
boxrig_ui_status boxrig_x11_input_adapter_reparent_client(
    boxrig_x11_input_adapter *adapter,
    Window new_parent);

#endif
