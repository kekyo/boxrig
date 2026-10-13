/* Installed optional packages must preserve the public C ABI and rollback. */
#include <boxrig/render/d3d11.h>
#ifdef BOXRIG_TEST_MANAGED_PACKAGE
#include <boxrig/win32/d3d11_component.h>
#endif
#include <stdio.h>

int main(void)
{
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_d3d11_renderer *renderer = NULL;
    if (boxrig_d3d11_renderer_create(&config, &renderer) != E_INVALIDARG ||
        renderer != NULL || FAILED(boxrig_d3d11_renderer_destroy(&renderer))) return 1;
#ifdef BOXRIG_TEST_MANAGED_PACKAGE
    {
        boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
        boxrig_win32_terminal_component_config component_config =
            boxrig_win32_terminal_component_config_default();
        boxrig_win32_terminal_component *component = NULL;
        if (graphics.warp_test_mode ||
            boxrig_win32_d3d11_terminal_component_create_child(NULL, NULL, NULL,
                &component_config, &graphics, &component) != BOXRIG_UI_INVALID_ARGUMENT ||
            component != NULL) return 1;
    }
#endif
    puts("PASS installed D3D11 package rollback");
    return 0;
}
