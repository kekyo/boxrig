/* Exercise automatic native fallback at the real sample's component boundary. */
#include "boxrig/win32/opengl_component.h"
#include <stdio.h>

boxrig_ui_status __wrap_boxrig_win32_opengl_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **out)
{
    (void)instance; (void)parent; (void)model; (void)config;
    *out = NULL;
    fputs("TEST OpenGL initialization unavailable\n", stderr);
    return BOXRIG_UI_UNAVAILABLE;
}
