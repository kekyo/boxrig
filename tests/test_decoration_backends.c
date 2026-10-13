/* boxrig - Under MIT. */
#include "selection_renderer_fixture.h"
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

int main(void)
{
    const boxrig_test_decoration_factory factories[] = {
        boxrig_test_create_pango_decoration, boxrig_test_create_opengl_decoration,
        boxrig_test_create_x11_decoration, boxrig_test_create_pango_decoration};
    const char *const names[] = {"PangoCairo", "OpenGL", "X11", "PangoCairo restored"};
    bool passed = boxrig_test_renderer_decoration_views(factories, names, 4);
    if (!passed) return 1;
    cairo_debug_reset_static_data();
    FcFini();
    return 0;
}
