/* Private DirectWrite shaping and raster interfaces shared by native consumers. */
#ifndef BOXRIG_WIN32_TEXT_PRIVATE_H
#define BOXRIG_WIN32_TEXT_PRIVATE_H
#include "renderer_private.h"
#include "boxrig/ui/image.h"
#include "../opengl/text_provider.h"

typedef struct boxrig_win32_text_face {
    IDWriteFontFace *face;
    float em_size;
} boxrig_win32_text_face;

int boxrig_win32_text_shape(boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal, size_t row, boxrig_ogl_row_shape *shape);
int boxrig_win32_text_shape_composition(boxrig_win32_renderer *renderer,
    const boxrig_composition *composition, size_t first_cluster, size_t cluster_count,
    boxrig_text_direction direction, boxrig_ogl_row_shape *shape);
void boxrig_win32_text_shape_destroy(boxrig_ogl_row_shape *shape);
int boxrig_win32_text_raster(boxrig_win32_renderer *renderer,
    const boxrig_ogl_shaped_item *item, size_t glyph_index, size_t limit,
    uint8_t **bytes, size_t *count, uint32_t *width, uint32_t *height,
    int32_t *bearing_x, int32_t *bearing_y, bool *color);
/* Native raster consumers share the complete shape and layer pipeline. Each
 * callback receives physical target coordinates and an explicit clip. */
typedef struct boxrig_win32_raster_sink {
    void *context;
    int (*fill)(void *, const RECT *, COLORREF, uint8_t);
    int (*glyph)(void *, const DWRITE_GLYPH_RUN *, float, float, float,
        float, const RECT *, COLORREF, DWRITE_MEASURING_MODE,
        IDWriteRenderingParams *, IDWriteFactory2 *);
    int (*image)(void *, const boxrig_terminal *, boxrig_image_handle,
        const boxrig_image_composite_operation *, size_t *);
    /* Source-over decoration, independent of the background replacement fill. */
    int (*over)(void *, const RECT *, COLORREF, uint8_t);
    /* Composite an application color behind an already restored terminal clip. */
    int (*behind)(void *, const RECT *, COLORREF, uint8_t);
    /* Immutable application image; independent of the terminal image cache. */
    int (*background_image)(void *, const RECT *, boxrig_ui_background_image_view,
        boxrig_ui_background_rectangle, const boxrig_ui_view *);
} boxrig_win32_raster_sink;

int boxrig_win32_text_create(boxrig_win32_renderer **renderer_out);
void boxrig_win32_text_set_sink(boxrig_win32_renderer *renderer,
    const boxrig_win32_raster_sink *sink, unsigned int width,
    unsigned int height, uint8_t opacity);
int boxrig_win32_text_draw_view(boxrig_win32_renderer *renderer,
    boxrig_ui_view *view, boxrig_composition *composition);
void boxrig_win32_text_invalidate(boxrig_win32_renderer *renderer);
bool boxrig_win32_text_color_layers(const boxrig_win32_renderer *renderer);
#endif
