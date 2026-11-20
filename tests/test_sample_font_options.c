/* Exercise the startup controls used by the Linux terminal samples. */
#define _POSIX_C_SOURCE 200809L
#include "../examples/boxrig_term/common/font_demo.h"
#include <assert.h>
#include <stdlib.h>

static boxrig_ui_status apply(void *context, const boxrig_term_font_demo *demo)
{
    boxrig_term_font_demo_update_policy(demo, context);
    return BOXRIG_UI_OK;
}

int main(void)
{
    const char *boxes[] = {"conservative", "legacy", "bulk"};
    const boxrig_box_drawing_mode modes[] = {BOXRIG_BOX_DRAWING_CONSERVATIVE,
        BOXRIG_BOX_DRAWING_LEGACY, BOXRIG_BOX_DRAWING_BULK};
    boxrig_term_font_demo demo;
    assert(unsetenv("BOXRIG_COLOR_GLYPHS") == 0);
    for (size_t i = 0; i < 3; i++) {
        assert(setenv("BOXRIG_BOX_DRAWING", boxes[i], 1) == 0);
        for (size_t gray = 0; gray <= 1; gray++) {
            assert(setenv("BOXRIG_FONT_ANTIALIAS", gray ? "grayscale" : "system", 1) == 0);
            boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
            policy.antialias = BOXRIG_FONT_ANTIALIAS_SUBPIXEL;
            policy.color_glyphs = false;
            assert(boxrig_term_font_demo_initialize(&demo, NULL, apply, &policy) == 0);
            assert(policy.box_drawing == modes[i] && !policy.color_glyphs);
            assert(policy.antialias == (gray ? BOXRIG_FONT_ANTIALIAS_GRAYSCALE : BOXRIG_FONT_ANTIALIAS_SYSTEM));
        }
    }
    boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
    assert(setenv("BOXRIG_FONT_ANTIALIAS", "bad", 1) == 0);
    assert(boxrig_term_font_demo_initialize(&demo, NULL, apply, &policy) == -1);
    return 0;
}
