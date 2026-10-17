/* boxrig - Under MIT. */
#include "measurement.h"
#include "gtk/gl_surface.h"
#include "opengl/text_provider.h"
#include "pangocairo/renderer.h"
#include "terminal_internal.h"

#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool measurement_detail = true;
measured_stage measurement_stages[BOXRIG_MEASURE_STAGE_COUNT];
boxrig_ogl_render_statistics measurement_gl_statistics;
size_t measurement_gl_draws;
size_t measurement_gtk_compositions;
uint64_t measurement_last_composition;

bool __real_boxrig_gtk_gl_surface_can_present_opaque(const boxrig_ui_view *);
bool __wrap_boxrig_gtk_gl_surface_can_present_opaque(const boxrig_ui_view *view)
{
    /* Diagnostic-only control: no public or production environment setting. */
    if (getenv("BOXRIG_MEASURE_RGBA") != NULL) return false;
    return __real_boxrig_gtk_gl_surface_can_present_opaque(view);
}

uint64_t measurement_clock(int clock_id)
{
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

boxrig_measurement_time boxrig_measurement_begin(void)
{
    boxrig_measurement_time time = {0};
    if (measurement_detail) {
        time.elapsed = measurement_clock(CLOCK_MONOTONIC);
        time.cpu = measurement_clock(CLOCK_THREAD_CPUTIME_ID);
    }
    return time;
}

void boxrig_measurement_end(boxrig_measurement_stage stage, boxrig_measurement_time start)
{
    if (!measurement_detail) return;
    measured_stage *value = &measurement_stages[stage];
    value->time.cpu += measurement_clock(CLOCK_THREAD_CPUTIME_ID) - start.cpu;
    value->time.elapsed += measurement_clock(CLOCK_MONOTONIC) - start.elapsed;
    value->calls++;
}

void measurement_reset(void)
{
    memset(measurement_stages, 0, sizeof(measurement_stages));
    measurement_gl_draws = 0;
    measurement_gtk_compositions = 0;
    measurement_last_composition = 0;
}

void measurement_print(void)
{
    static const char *names[] = {"feed", "recognition", "snapshot", "row_signature",
        "shape", "cairo_render", "cairo_allocate", "cairo_copy", "frame", "gl_draw",
        "gl_present", "gtk_import", "gtk_compose", "text_sync"};
    printf("\"stages\":{");
    for (size_t index = 0; index < BOXRIG_MEASURE_STAGE_COUNT; index++) {
        measured_stage stage = measurement_stages[index];
        printf("%s\"%s\":{\"calls\":%zu,\"elapsed_ms\":%.6f,\"cpu_ms\":%.6f}",
            index == 0 ? "" : ",", names[index], stage.calls,
            stage.time.elapsed / 1e6, stage.time.cpu / 1e6);
    }
    printf("}");
}

boxrig_status __real_boxrig_terminal_create_text_snapshot(const boxrig_terminal *,
    const boxrig_text_snapshot_options *, boxrig_text_snapshot **);
boxrig_status __wrap_boxrig_terminal_create_text_snapshot(const boxrig_terminal *terminal,
    const boxrig_text_snapshot_options *options, boxrig_text_snapshot **snapshot)
{
    BOXRIG_MEASURE_BEGIN(start);
    boxrig_status status = __real_boxrig_terminal_create_text_snapshot(terminal, options, snapshot);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_SNAPSHOT, start);
    return status;
}

void __real_boxrig_screen_text_sync(boxrig_screen *);
void __wrap_boxrig_screen_text_sync(boxrig_screen *screen)
{
    BOXRIG_MEASURE_BEGIN(start);
    __real_boxrig_screen_text_sync(screen);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_TEXT_SYNC, start);
}

int __real_boxrig_ogl_text_provider_shape_row(boxrig_ogl_text_provider *, const boxrig_terminal *,
    size_t, boxrig_ogl_row_shape *);
int __wrap_boxrig_ogl_text_provider_shape_row(boxrig_ogl_text_provider *provider,
    const boxrig_terminal *terminal, size_t row, boxrig_ogl_row_shape *shape)
{
    BOXRIG_MEASURE_BEGIN(start);
    int status = __real_boxrig_ogl_text_provider_shape_row(provider, terminal, row, shape);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_SHAPE, start);
    return status;
}

boxrig_ui_status __real_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *, boxrig_ui_view *,
    boxrig_composition *, boxrig_gtk_render_state *);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition, boxrig_gtk_render_state *state)
{
    static bool reported;
    if (!reported) {
        printf("{\"kind\":\"gpu\",\"vendor\":\"%s\",\"renderer\":\"%s\",\"version\":\"%s\"}\n",
            glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
        reported = true;
    }
    BOXRIG_MEASURE_BEGIN(start);
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_draw(surface, view, composition, state);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_GL_DRAW, start);
    measurement_gl_draws++;
    if (boxrig_gtk_gl_surface_get_statistics(surface, &measurement_gl_statistics) != 0) abort();
    return status;
}

boxrig_ui_status __real_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *,
    const boxrig_gtk_gl_destination *, boxrig_gtk_render_state *);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gtk_gl_destination *target, boxrig_gtk_render_state *state)
{
    static uint64_t generation;
    if (generation != target->generation) {
        GLint previous, alpha, encoding;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->draw_framebuffer);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alpha);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous);
        printf("{\"kind\":\"target\",\"width\":%u,\"height\":%u,\"format\":%d,"
            "\"encoding\":%d,\"alpha_bits\":%d,\"samples\":%u,\"generation\":%llu}\n",
            target->width, target->height,
            target->srgb ? (alpha == 0 ? GL_SRGB8 : GL_SRGB8_ALPHA8) : (alpha == 0 ? GL_RGB8 : GL_RGBA8), encoding,
            alpha, target->samples, (unsigned long long)target->generation);
        generation = target->generation;
    }
    BOXRIG_MEASURE_BEGIN(start);
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_present(surface, target, state);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_GL_PRESENT, start);
    return status;
}

int __real_boxrig_pango_cairo_renderer_draw_view_composition(boxrig_pango_cairo_renderer *,
    boxrig_ui_view *, boxrig_composition *);
int __wrap_boxrig_pango_cairo_renderer_draw_view_composition(boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view, boxrig_composition *composition)
{
    BOXRIG_MEASURE_BEGIN(start);
    int status = __real_boxrig_pango_cairo_renderer_draw_view_composition(renderer, view, composition);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_CAIRO_RENDER, start);
    return status;
}

/* Interposition observes the public GDK calls made inside GTK's shared
 * library. It neither changes GTK state nor inserts a GPU synchronization. */
void gdk_cairo_draw_from_gl(cairo_t *cr, GdkWindow *window, int source,
    int source_type, int scale, int x, int y, int width, int height)
{
    static void (*original)(cairo_t *, GdkWindow *, int, int, int, int, int, int, int);
    if (original == NULL) {
        void *symbol = dlsym(RTLD_NEXT, "gdk_cairo_draw_from_gl");
        if (symbol == NULL || sizeof(original) != sizeof(symbol)) abort();
        memcpy(&original, &symbol, sizeof(original));
    }
    BOXRIG_MEASURE_BEGIN(start);
    original(cr, window, source, source_type, scale, x, y, width, height);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_GTK_IMPORT, start);
}

void gdk_window_end_draw_frame(GdkWindow *window, GdkDrawingContext *context)
{
    static void (*original)(GdkWindow *, GdkDrawingContext *);
    if (original == NULL) {
        void *symbol = dlsym(RTLD_NEXT, "gdk_window_end_draw_frame");
        if (symbol == NULL || sizeof(original) != sizeof(symbol)) abort();
        memcpy(&original, &symbol, sizeof(original));
    }
    BOXRIG_MEASURE_BEGIN(start);
    original(window, context);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_GTK_COMPOSE, start);
    measurement_last_composition = measurement_clock(CLOCK_MONOTONIC);
    measurement_gtk_compositions++;
}
