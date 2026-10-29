/* Exercise installed X11 and Wayland public APIs through shared libraries. */
#if defined(BOXRIG_TEST_X11)
#include <boxrig/x11.h>
#include <boxrig/x11/component.h>
#include <boxrig/x11/event.h>
#elif defined(BOXRIG_TEST_WAYLAND)
#include <boxrig/wayland.h>
#else
#error Select a native frontend
#endif

#include <assert.h>
#include <stdio.h>
#include <boxrig/ui/animation_poll.h>

int main(void)
{
    boxrig_ui_animation_poll *adapter = NULL;
    int timeout = -2;
#if defined(BOXRIG_TEST_X11)
    boxrig_x11_terminal_component_config config =
        boxrig_x11_terminal_component_config_default();
    uint64_t milliseconds = 0;
    assert(config.paste_chunk_size > 0);
    assert(boxrig_x11_sample_monotonic_time(&milliseconds) == 0);
    assert(milliseconds > 0);
    assert(boxrig_x11_animation_poll_create(&adapter) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == -1);
    puts("PASS installed shared X11 APIs");
#else
    boxrig_wayland_context_config context =
        boxrig_wayland_context_config_default();
    boxrig_wayland_terminal_component_config component =
        boxrig_wayland_terminal_component_config_default();
    assert(context.maximum_components > 0);
    assert(component.buffer_count >= BOXRIG_WAYLAND_BUFFERS_MINIMUM);
    assert(component.buffer_count <= BOXRIG_WAYLAND_BUFFERS_MAXIMUM);
    assert(boxrig_wayland_animation_poll_create(&adapter) == BOXRIG_UI_OK);
    assert(boxrig_ui_animation_poll_prepare_wait(adapter, -1, &timeout) == BOXRIG_UI_OK);
    assert(timeout == -1);
    puts("PASS installed shared Wayland APIs");
#endif
    boxrig_ui_animation_poll_destroy(adapter);
    return 0;
}
