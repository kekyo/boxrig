/* boxrig - Under MIT. */
#define GL_GLEXT_PROTOTYPES 1
#include "blink_renderer_fixture.h"
#include "selection_renderer_fixture.h"
#include "opengl_host.h"
#include "boxrig/render/opengl.h"

#include <GL/gl.h>
#include <GL/glext.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct color_host {
    opengl_host *context;
    boxrig_ogl_renderer *renderer;
    uint32_t framebuffer;
    uint32_t texture;
    boxrig_ogl_render_statistics baseline;
} color_host;

static int draw(void *context, boxrig_ui_view *view,
    boxrig_composition *composition)
{
    color_host *host = context;
    int result = composition == NULL
        ? boxrig_ogl_renderer_draw_view(host->renderer, view)
        : boxrig_ogl_renderer_draw_view_composition(host->renderer, view, composition);
    if (result != 0) {
        boxrig_ogl_render_statistics state;
        if (boxrig_ogl_renderer_get_render_statistics(host->renderer, &state) == 0) {
            fprintf(stderr, "OpenGL fixture draw failed: in-flight frames=%zu\n",
                state.in_flight_frame_count);
        }
    }
    return result;
}

static uint32_t *capture(void *context)
{
    color_host *host = context;
    size_t count = BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT;
    uint8_t *rgba = malloc(count * 4);
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    if (rgba == NULL || pixels == NULL) {
        free(rgba);
        free(pixels);
        return NULL;
    }
    GLint previous = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, host->framebuffer);
    /* A new fixture context retains the default tightly packed RGBA state. */
    glReadPixels(0, 0, BOXRIG_TEST_COLOR_WIDTH, BOXRIG_TEST_COLOR_HEIGHT,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    GLenum error = glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous);
    if (error != GL_NO_ERROR) {
        free(rgba);
        free(pixels);
        return NULL;
    }
    for (size_t y = 0; y < BOXRIG_TEST_COLOR_HEIGHT; y++) {
        const uint8_t *row = rgba +
            (BOXRIG_TEST_COLOR_HEIGHT - y - 1) * BOXRIG_TEST_COLOR_WIDTH * 4;
        for (size_t x = 0; x < BOXRIG_TEST_COLOR_WIDTH; x++) {
            pixels[y * BOXRIG_TEST_COLOR_WIDTH + x] =
                ((uint32_t)row[4 * x] << 16) |
                ((uint32_t)row[4 * x + 1] << 8) | row[4 * x + 2];
        }
    }
    free(rgba);
    return pixels;
}

static void reset_statistics(void *context)
{
    color_host *host = context;
    if (boxrig_ogl_renderer_get_render_statistics(host->renderer,
            &host->baseline) == -1) abort();
}

static int statistics(void *context, boxrig_test_color_statistics *statistics)
{
    color_host *host = context;
    boxrig_ogl_render_statistics actual;
    if (boxrig_ogl_renderer_get_render_statistics(host->renderer, &actual) == -1) return -1;
    *statistics = (boxrig_test_color_statistics){
        actual.row_shape_count - host->baseline.row_shape_count,
        actual.row_raster_count - host->baseline.row_raster_count,
        actual.grayscale_glyph_upload_count - host->baseline.grayscale_glyph_upload_count +
        actual.color_glyph_upload_count - host->baseline.color_glyph_upload_count};
    return 0;
}

static void destroy(void *context)
{
    color_host *host = context;
    if (host->renderer != NULL &&
        boxrig_ogl_renderer_destroy(&host->renderer, 1, NULL) != BOXRIG_GL_OK) abort();
    if (host->context != NULL) {
        opengl_host_delete_framebuffer(host->context, host->framebuffer, host->texture);
        opengl_host_destroy(host->context);
    }
    free(host);
}

static float cursor_style_scale = 1;

static bool create(boxrig_test_color_renderer *renderer)
{
    color_host *host = calloc(1, sizeof(*host));
    if (host == NULL) return false;
    opengl_host_report report;
    if (!opengl_host_create("egl", &host->context, &report)) {
        destroy(host);
        return false;
    }
    host->framebuffer = opengl_host_create_framebuffer(host->context,
        BOXRIG_GL_TARGET_RGBA8, BOXRIG_TEST_COLOR_WIDTH, BOXRIG_TEST_COLOR_HEIGHT,
        &host->texture);
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    if (cursor_style_scale != 1) {
        config.rendering_policy.dpi_x = 144;
        config.rendering_policy.dpi_y = 144;
        config.rendering_policy.output_scale = cursor_style_scale / 1.5f;
    }
    config.runtime.resolver_context = host->context;
    config.runtime.resolve = opengl_host_resolve;
    config.runtime.current_context_query_context = host->context;
    config.runtime.query_current_context = opengl_host_query_current;
    config.runtime.context_generation = 1;
    config.runtime.target_generation = 1;
    config.initial_target = (boxrig_gl_target){0};
    config.initial_target.draw_framebuffer = host->framebuffer;
    config.initial_target.width = BOXRIG_TEST_COLOR_WIDTH;
    config.initial_target.height = BOXRIG_TEST_COLOR_HEIGHT;
    config.initial_target.generation = 1;
    config.initial_target.format = BOXRIG_GL_TARGET_RGBA8;
    config.initial_target.alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED;
    config.initial_target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    if (host->framebuffer == 0 || boxrig_ogl_renderer_create(&config,
            &host->renderer, NULL) != BOXRIG_GL_OK) {
        destroy(host);
        return false;
    }
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_ogl_renderer_cell_width(host->renderer),
        boxrig_ogl_renderer_cell_height(host->renderer),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

/* Reuse the observable capture adapter for renderer-switch integration. */
bool boxrig_test_create_opengl_decoration(boxrig_test_color_renderer *renderer)
{
    cursor_style_scale = 1;
    return create(renderer);
}

int main(void)
{
    if (!boxrig_test_renderer_filled_cursor(create, 1)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 3, 3)) return 1;
    if (!boxrig_test_renderer_cursor_style(create)) return 1;
    bool passed = (boxrig_test_renderer_cursor_blink(create) && (boxrig_test_renderer_text_blink(create) && boxrig_test_renderer_blink_composition(create)));
    if (!passed) return 1;
    cursor_style_scale = 2;
    if (!boxrig_test_renderer_filled_cursor(create, 2)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 6, 6)) return 1;
    if (!boxrig_test_renderer_cursor_style_at_scale(create, 6, 6)) return 1;
    cursor_style_scale = 1.5f;
    if (!boxrig_test_renderer_cursor_style_at_scale(create, 5, 5)) return 1;
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS OpenGL cursor blink fixture");
    return 0;
}
