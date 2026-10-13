/* boxrig - Under MIT. */
#ifndef BOXRIG_TEST_COLOR_RENDERER_FIXTURE_H
#define BOXRIG_TEST_COLOR_RENDERER_FIXTURE_H

#include "boxrig/ui.h"

#define BOXRIG_TEST_COLOR_WIDTH 600
#define BOXRIG_TEST_COLOR_HEIGHT 200

typedef struct boxrig_test_color_statistics {
    size_t row_shape_count;
    size_t row_raster_count;
    size_t glyph_upload_count;
} boxrig_test_color_statistics;

/* Backend adapters supply a 600x200 surface and native cell metrics. Captures
 * contain owned, top-to-bottom RGB words and are released with free(). */
typedef struct boxrig_test_color_renderer {
    void *context;
    unsigned int cell_width;
    unsigned int cell_height;
    int (*draw)(void *context, boxrig_ui_view *view,
        boxrig_composition *composition);
    uint32_t *(*capture)(void *context);
    void (*reset_statistics)(void *context);
    int (*statistics)(void *context, boxrig_test_color_statistics *statistics);
    void (*destroy)(void *context);
} boxrig_test_color_renderer;

/* Tests observable color behavior, identity rendering and retained shapes. */
bool boxrig_test_renderer_colors(
    bool (*create)(boxrig_test_color_renderer *renderer));

#endif
