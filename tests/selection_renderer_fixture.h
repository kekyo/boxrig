/* boxrig - Under MIT. */
#ifndef BOXRIG_TEST_SELECTION_RENDERER_FIXTURE_H
#define BOXRIG_TEST_SELECTION_RENDERER_FIXTURE_H
#include "color_renderer_fixture.h"

/* Observe selection colors and alpha through the same native capture adapters. */
bool boxrig_test_renderer_selection_style(
    bool (*create)(boxrig_test_color_renderer *renderer),
    unsigned int expected_stroke_x, unsigned int expected_stroke_y);

typedef bool (*boxrig_test_decoration_factory)(boxrig_test_color_renderer *renderer);

/* Each factory takes ownership of its native target until destroy. The fixture
 * preserves one model and two independently decorated views across replacements. */
bool boxrig_test_renderer_decoration_views(
    const boxrig_test_decoration_factory *factories,
    const char *const *names, size_t factory_count);

bool boxrig_test_create_pango_decoration(boxrig_test_color_renderer *renderer);
bool boxrig_test_create_opengl_decoration(boxrig_test_color_renderer *renderer);
bool boxrig_test_create_x11_decoration(boxrig_test_color_renderer *renderer);
bool boxrig_test_create_win32_decoration(boxrig_test_color_renderer *renderer);
#endif
