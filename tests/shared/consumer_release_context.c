/* Exercise installed public interfaces that need no display or GL context. */
#include <assert.h>
#include <stdio.h>

#if defined(BOXRIG_TEST_RENDER_OGL)
#include <boxrig/render/opengl.h>
#include <boxrig/render/runtime.h>

int main(void)
{
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    boxrig_ogl_renderer *renderer = NULL;
    boxrig_gl_diagnostic diagnostic;
    assert(config.limits.maximum_glyph_entries > 0);
    assert(boxrig_ogl_renderer_create(&config, &renderer, &diagnostic) ==
           BOXRIG_GL_INVALID_ARGUMENT);
    assert(renderer == NULL);
    assert(diagnostic.status == BOXRIG_GL_INVALID_ARGUMENT);
    assert(boxrig_gl_runtime_set_clip(NULL, NULL, NULL) ==
           BOXRIG_GL_INVALID_ARGUMENT);
    puts("PASS installed OpenGL renderer diagnostics without a context");
    return 0;
}

#elif defined(BOXRIG_TEST_BACKGROUND_GTK3)
#include <boxrig/gtk3/background_effect.h>

int main(void)
{
    boxrig_gtk3_background_effect *effect = NULL;
    assert(boxrig_gtk3_background_effect_create(NULL, NULL, NULL, &effect) ==
           BOXRIG_UI_INVALID_ARGUMENT);
    assert(effect == NULL);
    boxrig_gtk3_background_effect_destroy(effect);
    puts("PASS installed GTK 3 background adapter without a display");
    return 0;
}

#elif defined(BOXRIG_TEST_BACKGROUND_GTK4)
#include <boxrig/gtk4/background_effect.h>

int main(void)
{
    boxrig_gtk4_background_effect *effect = NULL;
    assert(boxrig_gtk4_background_effect_create(NULL, NULL, NULL, &effect) ==
           BOXRIG_UI_INVALID_ARGUMENT);
    assert(effect == NULL);
    boxrig_gtk4_background_effect_destroy(effect);
    puts("PASS installed GTK 4 background adapter without a display");
    return 0;
}

#else
#error Select a context-free release consumer
#endif
