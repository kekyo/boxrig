/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/opengl_component.h"

#include <stdio.h>

int main(void)
{
    boxrig_x11_opengl_component_config config =
        boxrig_x11_opengl_component_config_default();

    if (config.swap_interval != 1) {
        return 1;
    }
    printf("PASS installed X11 OpenGL package compiles links and runs\n");
    return 0;
}
