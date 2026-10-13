/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/wayland.h"

#include <stdio.h>

int main(void)
{
    boxrig_wayland_context_config context_config =
        boxrig_wayland_context_config_default();
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();

    if (context_config.maximum_components == 0 ||
        context_config.maximum_work_items == 0 ||
        component_config.buffer_count < BOXRIG_WAYLAND_BUFFERS_MINIMUM ||
        component_config.buffer_count > BOXRIG_WAYLAND_BUFFERS_MAXIMUM ||
        component_config.maximum_bytes == 0 ||
        component_config.maximum_dirty_rectangles == 0) {
        return 1;
    }
    printf("PASS installed native Wayland package compiles links and runs\n");
    return 0;
}
