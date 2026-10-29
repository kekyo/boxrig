/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#ifdef BOXRIG_PLAN18_CAIRO
#define BOXRIG_TEST_FRONTEND_PANGO_CAIRO 1
#else
#define BOXRIG_TEST_FRONTEND_OPENGL 1
#include "boxrig/render/opengl.h"
#include "boxrig/render/runtime.h"
static boxrig_ogl_renderer_config plan18_config_default(void);
#define boxrig_ogl_renderer_config_default plan18_config_default
#endif
#define main boxrig_fixture_main
#include "../test_ui_platform_fixture.c"
#undef main
#ifndef BOXRIG_PLAN18_CAIRO
#undef boxrig_ogl_renderer_config_default
#endif
#include <inttypes.h>
#include <time.h>

#ifndef BOXRIG_PLAN18_CAIRO
static boxrig_gl_runtime *plan18_runtime;

boxrig_gl_status __real_boxrig_gl_runtime_create(
    const boxrig_gl_runtime_config *config, boxrig_gl_runtime **runtime_out,
    boxrig_gl_diagnostic *diagnostic_out);

boxrig_gl_status __wrap_boxrig_gl_runtime_create(
    const boxrig_gl_runtime_config *config, boxrig_gl_runtime **runtime_out,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_gl_status status = __real_boxrig_gl_runtime_create(config, runtime_out, diagnostic_out);
    if (status == BOXRIG_GL_OK) plan18_runtime = *runtime_out;
    return status;
}

static boxrig_ogl_renderer_config plan18_config_default(void)
{
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    config.font_size_dip = 23;
    return config;
}
#endif

static uint64_t clock_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

int main(void)
{
    platform_renderer_fixture renderer;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    config.rows = 45;
    config.columns = 193;
    if (!create_platform_renderer(&renderer)) return 1;
#ifdef BOXRIG_PLAN18_CAIRO
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
    font.size_dip = 23;
    if (boxrig_pango_cairo_renderer_set_font(renderer.renderer, &font, &policy) !=
        BOXRIG_FONT_CATALOG_OK) return 1;
    renderer.cell_width = boxrig_pango_cairo_renderer_cell_width(renderer.renderer);
    renderer.cell_height = boxrig_pango_cairo_renderer_cell_height(renderer.renderer);
#endif
    if (!size_platform_renderer(&renderer, config.rows, config.columns) ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        !feed_bytes(terminal, "\033[?25l", 6) ||
        !draw_platform_view(&renderer, view, NULL)) return 1;
#ifdef BOXRIG_PLAN18_CAIRO
    puts("case\titeration\tcpu_ns\trows\tcells");
#else
    puts("case\titeration\tcpu_ns\trows\tcells\tbatches\tinstances\tuploads\tglyph_uploads\ttarget_checks\tattribute_pointers\tuniform_lookups\tstate_queries\tstate_changes\tstate_restores");
#endif
    for (unsigned int kind = 0; kind < 5; kind++) {
        for (unsigned int iteration = 0; iteration < 20; iteration++) {
            const char *name = kind == 0 ? "cold_one_row" :
                kind == 1 ? "warm_one_row" : kind == 2 ? "three_rows" :
                kind == 3 ? "all_rows_dirty" : "full_frame";
            if (kind == 0) {
                char sequence[32];
                int length = snprintf(sequence, sizeof(sequence),
                    "\033[1;1H%c", (int)('A' + iteration));
                if (length <= 0 || !feed_bytes(terminal, sequence, (size_t)length)) return 1;
            } else if (kind == 1) {
                char sequence[32];
                int length = snprintf(sequence, sizeof(sequence),
                    "\033[1;2H%c", (int)('A' + iteration));
                if (length <= 0 || !feed_bytes(terminal, sequence, (size_t)length)) return 1;
            } else if (kind == 2) {
                const char *sequence = iteration % 2 == 0 ?
                    "\033[1;1H\033[41m  \033[3;1H\033[42m  \033[5;1H\033[44m  " :
                    "\033[1;1H\033[44m  \033[3;1H\033[41m  \033[5;1H\033[42m  ";
                if (!feed_bytes(terminal, sequence, strlen(sequence))) return 1;
            } else if (kind == 3) {
                boxrig_ui_dirty dirty = {0, config.rows, 0, config.columns};
                if (boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) != BOXRIG_UI_OK)
                    return 1;
            } else {
#ifdef BOXRIG_PLAN18_CAIRO
                unsigned int width = config.columns * renderer.cell_width;
                unsigned int height = config.rows * renderer.cell_height;
                if (boxrig_pango_cairo_renderer_set_surface_size(renderer.renderer,
                        width + 1, height) != 0 ||
                    boxrig_pango_cairo_renderer_set_surface_size(renderer.renderer,
                        width, height) != 0) return 1;
#else
                if (boxrig_ogl_renderer_target_lost(renderer.renderer, view) != BOXRIG_RENDERER_OK)
                    return 1;
#endif
            }
            if (boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK) return 1;
#ifdef BOXRIG_PLAN18_CAIRO
            boxrig_pango_cairo_render_statistics before, after;
            if (boxrig_pango_cairo_renderer_get_render_statistics(renderer.renderer, &before) != 0)
                return 1;
#else
            boxrig_ogl_render_statistics before, after;
            if (boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &before) != 0) return 1;
            opengl_host_work work_before = opengl_host_get_work(renderer.host);
            boxrig_gl_state_counters state_before, state_after;
            if (plan18_runtime == NULL || boxrig_gl_runtime_get_state_counters(
                    plan18_runtime, &state_before) != BOXRIG_GL_OK) return 1;
#endif
            uint64_t started = clock_ns();
            if (!draw_platform_view(&renderer, view, NULL)) return 1;
            uint64_t elapsed = clock_ns() - started;
#ifdef BOXRIG_PLAN18_CAIRO
            cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer.renderer);
            if (surface == NULL) return 1;
            cairo_surface_flush(surface);
            if (boxrig_pango_cairo_renderer_get_render_statistics(renderer.renderer, &after) != 0)
                return 1;
            printf("%s\t%u\t%" PRIu64 "\t%zu\t%zu\n", name, iteration, elapsed,
                after.row_raster_count - before.row_raster_count,
                after.cell_raster_count - before.cell_raster_count);
#else
            uint8_t pixel[4];
            if (!opengl_host_read_pixel(renderer.host, renderer.target.draw_framebuffer,
                    renderer.target.height, 0, 0, pixel)) return 1;
            opengl_host_work work_after = opengl_host_get_work(renderer.host);
            if (boxrig_gl_runtime_get_state_counters(plan18_runtime, &state_after) != BOXRIG_GL_OK)
                return 1;
            if (boxrig_ogl_renderer_get_render_statistics(renderer.renderer, &after) != 0) return 1;
            printf("%s\t%u\t%" PRIu64 "\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n",
                name, iteration, elapsed,
                after.row_raster_count - before.row_raster_count,
                after.cell_raster_count - before.cell_raster_count,
                after.draw_batch_count - before.draw_batch_count,
                after.instance_count - before.instance_count,
                after.upload_bytes - before.upload_bytes,
                after.grayscale_glyph_upload_count + after.color_glyph_upload_count -
                    before.grayscale_glyph_upload_count - before.color_glyph_upload_count,
                work_after.target_checks - work_before.target_checks,
                work_after.attribute_pointers - work_before.attribute_pointers,
                work_after.uniform_lookups - work_before.uniform_lookups,
                state_after.query_count - state_before.query_count,
                state_after.change_count - state_before.change_count,
                state_after.restore_count - state_before.restore_count);
#endif
        }
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    destroy_platform_renderer(&renderer);
    return 0;
}
