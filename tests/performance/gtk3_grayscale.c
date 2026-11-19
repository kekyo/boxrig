/* Make the GTK3 performance sample choose explicit grayscale antialiasing. */
#include "boxrig/ui/text_layout.h"

boxrig_font_rendering_policy __real_boxrig_font_rendering_policy_default(void);

boxrig_font_rendering_policy __wrap_boxrig_font_rendering_policy_default(void)
{
    boxrig_font_rendering_policy policy = __real_boxrig_font_rendering_policy_default();
    policy.antialias = BOXRIG_FONT_ANTIALIAS_GRAYSCALE;
    return policy;
}
