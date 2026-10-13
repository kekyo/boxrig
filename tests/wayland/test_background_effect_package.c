/* boxrig - Under MIT. Installed standalone Wayland provider consumer. */
#include <boxrig/wayland/background_effect.h>
#include <assert.h>
#include <stdio.h>

int main(void)
{
    boxrig_wayland_background_effect *effect;
    assert(boxrig_wayland_background_effect_create(NULL, &effect) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(effect == NULL);
    assert(!boxrig_wayland_background_effect_ready(effect));
    assert(!boxrig_wayland_background_effect_get_state(effect).supported);
    boxrig_wayland_background_effect_destroy(effect);
    puts("PASS installed standalone Wayland background provider links without GTK");
    return 0;
}
