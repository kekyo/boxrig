/* Native automatic animation needs an explicit X11 loop - Under MIT. */
#include "boxrig/x11/component.h"

#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <assert.h>
#include <stdio.h>

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    assert(display != NULL);
    Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display),
        0, 0, 240, 120, 0, 0, 0);
    assert(parent != None);
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component *component = NULL;
    boxrig_x11_terminal_component_config config = boxrig_x11_terminal_component_config_default();
    assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    /* Default blinking must not silently stay steady without a host scheduler. */
    assert(boxrig_x11_terminal_component_create_child(display, parent, model,
        &config, &component) == BOXRIG_UI_UNAVAILABLE);
    assert(component == NULL);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    XDestroyWindow(display, parent);
    XCloseDisplay(display);
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS X11 rejects automatic animation without an event-loop scheduler");
    return 0;
}
