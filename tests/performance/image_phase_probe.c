/* Test-only CPU intervals and image identities for fixed Kitty RGBA replay. */
#define _POSIX_C_SOURCE 200809L
#include "../../src/core/graphics.h"
#include "../../src/ui/renderer_lifecycle.h"
#include "boxrig/render/runtime.h"
#include <glib.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef BOXRIG_IMAGE_GTK
#include "../../src/gtk/gl_surface.h"
#include "../../src/pangocairo/renderer.h"
#include "boxrig/ui/image.h"
#include <gtk/gtk.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#endif

enum phase { FEED, DECODE, DRAW, CONVERT, COMPOSITE, UPLOAD, COPY, PRESENT, PHASE_COUNT };
static const char *names[] = {"feed", "decode", "draw", "convert", "composite", "upload", "copy", "present"};
static struct { uint64_t calls, units, cpu_ns; } totals[PHASE_COUNT];
static FILE *report;
static int sequence = -1;
static bool initialized;
static bool verify_all;
static boxrig_kitty_graphics_consumer original;

static uint64_t timestamp(clockid_t clock)
{
    struct timespec value;
    assert(clock_gettime(clock, &value) == 0);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void add(enum phase phase, uint64_t start, size_t units)
{
    totals[phase].cpu_ns += timestamp(CLOCK_THREAD_CPUTIME_ID) - start;
    totals[phase].calls++;
    totals[phase].units += units;
}

static void event(const char *kind)
{
    if (sequence >= 0)
        fprintf(report, "%s\t%d\t%" PRIu64 "\n", kind, sequence, timestamp(CLOCK_MONOTONIC));
}

static void submitted(void)
{
    if (sequence < 0) return;
    event("submit");
    for (size_t i = 0; i < PHASE_COUNT; i++)
        fprintf(report, "phase\t%d\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n",
            sequence, names[i], totals[i].calls, totals[i].units, totals[i].cpu_ns);
    fflush(report);
}

/* The sample uses the core protocol consumer. Time its payload callbacks
 * separately from APC headers, resource publication and terminal dispatch. */
static boxrig_status decode_run(void *context, const uint8_t *bytes, size_t length, size_t *used)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    boxrig_status result = original.put_run(context, bytes, length, used);
    add(DECODE, start, *used);
    return result;
}

static boxrig_status decode_byte(void *context, uint8_t byte)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    boxrig_status result = original.put(context, byte);
    add(DECODE, start, result == BOXRIG_STATUS_OK);
    return result;
}

void __real_boxrig_kitty_graphics_set_consumer(boxrig_kitty_graphics_state *, const boxrig_kitty_graphics_consumer *);
void __wrap_boxrig_kitty_graphics_set_consumer(boxrig_kitty_graphics_state *state,
    const boxrig_kitty_graphics_consumer *consumer)
{
    if (consumer == NULL || consumer->put_run == NULL || consumer->put == NULL) {
        __real_boxrig_kitty_graphics_set_consumer(state, consumer);
        return;
    }
    if (initialized) {
        assert(original.put_run == consumer->put_run && original.put == consumer->put);
    } else {
        const char *path = getenv("BOXRIG_IMAGE_WORK_PATH");
        assert(path != NULL && (report = fopen(path, "w")) != NULL);
        original = *consumer;
        verify_all = getenv("BOXRIG_VERIFY_ALL_IMAGES") != NULL;
        initialized = true;
    }
    boxrig_kitty_graphics_consumer timed = *consumer;
    timed.put_run = decode_run;
    timed.put = decode_byte;
    __real_boxrig_kitty_graphics_set_consumer(state, &timed);
}

static void observe_state(boxrig_terminal *terminal)
{
    if (report == NULL || sequence >= 23 || boxrig_terminal_synchronized_output(terminal)) return;
    boxrig_cell_view cell;
    if (boxrig_terminal_get_cell(terminal, 0, 0, &cell) != BOXRIG_STATUS_OK ||
        cell.background.kind != BOXRIG_COLOR_RGB || cell.background.red != 64 + 5 * (sequence + 1) ||
        cell.background.green != 80 || cell.background.blue != 80) return;
    sequence++;
    event("committed");
    size_t images = boxrig_terminal_image_count(terminal);
    size_t placements = boxrig_terminal_image_placement_count(terminal);
    fprintf(report, "state\t%d\t%zu\t%zu\n", sequence, images, placements);
    for (size_t i = 0; i < images; i++) {
        boxrig_image_handle handle;
        boxrig_image_view image;
        assert(boxrig_terminal_image_handle_at(terminal, i, &handle) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_get_image(terminal, handle, &image) == BOXRIG_STATUS_OK);
        char *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, image.rgba, image.rgba_length);
        assert(digest != NULL);
        fprintf(report, "image\t%d\t%u\t%zu\t%zu\t%zu\t%s\n",
            sequence, image.image_id, image.width, image.height, image.rgba_length, digest);
        g_free(digest);
    }
    for (size_t i = 0; i < placements; i++) {
        boxrig_image_placement_view p;
        assert(boxrig_terminal_get_image_placement(terminal, i, &p) == BOXRIG_STATUS_OK);
        fprintf(report, "place\t%d\t%u\t%u\t%zu\t%zu\t%u\t%u\t%zu\t%zu\t%zu\t%zu\t"
            "%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%d\n", sequence, p.image_id, p.placement_id,
            p.visual_row, p.visual_column, p.cell_y_offset, p.cell_x_offset, p.source_x, p.source_y,
            p.source_width, p.source_height, p.destination_pixel_width, p.destination_pixel_height,
            p.clip_x, p.clip_y, p.clip_width, p.clip_height, p.z_index);
    }
    fflush(report);
}

/* The xray producer publishes the next resource before removing its previous
 * one. Only hash each retained identity when first observed. Do this outside
 * the feed interval in a separate integrity run. */
static void observe_publications(boxrig_terminal *terminal)
{
    static uint64_t revision, last_placement;
    static boxrig_image_handle retained[2];
    static size_t retained_count;
    uint64_t current = boxrig_terminal_image_revision(terminal);
    if (!verify_all || revision == current) return;
    revision = current;
    boxrig_image_handle visible[2];
    size_t n = boxrig_terminal_image_count(terminal);
    assert(n <= G_N_ELEMENTS(visible));
    for (size_t i = 0; i < n; i++) {
        assert(boxrig_terminal_image_handle_at(terminal, i, &visible[i]) == BOXRIG_STATUS_OK);
        bool known = false;
        for (size_t j = 0; j < retained_count; j++) known |= retained[j] == visible[i];
        if (known) continue;
        boxrig_image_view image;
        assert(boxrig_terminal_get_image(terminal, visible[i], &image) == BOXRIG_STATUS_OK);
        char *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, image.rgba, image.rgba_length);
        assert(digest != NULL);
        fprintf(report, "resource\tall\t%u\t%zu\t%zu\t%s\n", image.image_id, image.width, image.height, digest);
        g_free(digest);
    }
    for (size_t i = 0; i < n; i++) retained[i] = visible[i];
    retained_count = n;
    for (size_t i = 0; i < boxrig_terminal_image_placement_count(terminal); i++) {
        boxrig_image_placement_view p;
        assert(boxrig_terminal_get_image_placement(terminal, i, &p) == BOXRIG_STATUS_OK);
        if (p.placement_creation_serial <= last_placement) continue;
        last_placement = p.placement_creation_serial;
        fprintf(report, "placement\tall\t%u\t%u\t%zu\t%zu\t%zu\t%zu\n",
            p.image_id, p.placement_id, p.visual_row, p.visual_column, p.destination_pixel_width, p.destination_pixel_height);
    }
}

boxrig_status __real_boxrig_terminal_feed(boxrig_terminal *, const uint8_t *, size_t, size_t *);
boxrig_status __wrap_boxrig_terminal_feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t count, size_t *used)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    boxrig_status result = __real_boxrig_terminal_feed(terminal, bytes, count, used);
    add(FEED, start, *used);
    observe_publications(terminal);
    observe_state(terminal);
    return result;
}

boxrig_gl_status __real_boxrig_gl_runtime_upload_texture(boxrig_gl_runtime *, uint32_t, boxrig_gl_texture_format,
    uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *, size_t, boxrig_gl_diagnostic *);
boxrig_gl_status __wrap_boxrig_gl_runtime_upload_texture(boxrig_gl_runtime *runtime, uint32_t texture,
    boxrig_gl_texture_format format, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *bytes, size_t length, boxrig_gl_diagnostic *diagnostic)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    boxrig_gl_status result = __real_boxrig_gl_runtime_upload_texture(runtime, texture, format,
        x, y, w, h, bytes, length, diagnostic);
    add(UPLOAD, start, length);
    return result;
}

boxrig_renderer_status __real_boxrig_renderer_lifecycle_prepare_frame(boxrig_renderer_lifecycle *, boxrig_ui_view *,
    boxrig_composition *, uint64_t, boxrig_renderer_frame_token *);
boxrig_renderer_status __wrap_boxrig_renderer_lifecycle_prepare_frame(boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view, boxrig_composition *composition, uint64_t generation, boxrig_renderer_frame_token *token)
{
#ifndef BOXRIG_IMAGE_GTK
    event("request");
#endif
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    boxrig_renderer_status status = __real_boxrig_renderer_lifecycle_prepare_frame(lifecycle, view,
        composition, generation, token);
    add(DRAW, start, 1);
    return status;
}

#ifdef BOXRIG_IMAGE_GTK
guint __real_gtk_widget_add_tick_callback(GtkWidget *, GtkTickCallback, gpointer, GDestroyNotify);
guint __wrap_gtk_widget_add_tick_callback(GtkWidget *widget, GtkTickCallback callback, gpointer data, GDestroyNotify destroy)
{
    event("request");
    return __real_gtk_widget_add_tick_callback(widget, callback, data, destroy);
}

void __real_gtk_gl_area_queue_render(GtkGLArea *);
void __wrap_gtk_gl_area_queue_render(GtkGLArea *area)
{
    event("request");
    __real_gtk_gl_area_queue_render(area);
}

int __real_boxrig_pango_cairo_renderer_draw_view_composition(boxrig_pango_cairo_renderer *, boxrig_ui_view *,
    boxrig_composition *);
int __wrap_boxrig_pango_cairo_renderer_draw_view_composition(boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view, boxrig_composition *composition)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    int result = __real_boxrig_pango_cairo_renderer_draw_view_composition(renderer, view, composition);
    add(DRAW, start, 1);
    return result;
}

bool __real_boxrig_premultiplied_image_create(const boxrig_image_view *, boxrig_premultiplied_image *);
bool __wrap_boxrig_premultiplied_image_create(const boxrig_image_view *view, boxrig_premultiplied_image *image)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    bool result = __real_boxrig_premultiplied_image_create(view, image);
    add(CONVERT, start, view->rgba_length);
    return result;
}

bool __real_boxrig_composite_premultiplied_image(uint32_t *, size_t, size_t, size_t,
    const boxrig_premultiplied_image *, const boxrig_image_composite_operation *, size_t *);
bool __wrap_boxrig_composite_premultiplied_image(uint32_t *pixels, size_t w, size_t h, size_t stride,
    const boxrig_premultiplied_image *image, const boxrig_image_composite_operation *operation, size_t *count)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    bool result = __real_boxrig_composite_premultiplied_image(pixels, w, h, stride, image, operation, count);
    add(COMPOSITE, start, result ? *count : 0);
    return result;
}

/* GTK3 creates its immutable ARGB image between retrieving the renderer's
 * surface and marking the new copy dirty. No GTK main-loop dispatch occurs. */
static uint64_t copy_start;
cairo_surface_t *__real_boxrig_pango_cairo_renderer_surface(boxrig_pango_cairo_renderer *);
cairo_surface_t *__wrap_boxrig_pango_cairo_renderer_surface(boxrig_pango_cairo_renderer *renderer)
{
    copy_start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    return __real_boxrig_pango_cairo_renderer_surface(renderer);
}

void __real_cairo_surface_mark_dirty(cairo_surface_t *);
void __wrap_cairo_surface_mark_dirty(cairo_surface_t *surface)
{
    __real_cairo_surface_mark_dirty(surface);
    if (copy_start != 0) {
        add(COPY, copy_start, (size_t)cairo_image_surface_get_width(surface) * cairo_image_surface_get_height(surface));
        copy_start = 0;
        submitted();
    }
}

boxrig_ui_status __real_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *, const boxrig_gl_target *,
    boxrig_gtk_render_state *);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface, const boxrig_gl_target *target,
    boxrig_gtk_render_state *state)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_present(surface, target, state);
    add(PRESENT, start, 1);
    if (status == BOXRIG_UI_OK) submitted();
    return status;
}
#else
/* Capture the complete terminal rectangle before EGL releases the back
 * buffer. Store its RGB digest in the same top-down order as X11's PPM. */
static char *framebuffer_digest(GLint x, GLint y, GLsizei width, GLsizei height)
{
    GLint pack_alignment, row_length, skip_rows, skip_pixels, pack_buffer;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    size_t stride = (size_t)width * 4;
    uint8_t *rgba = g_malloc_n((size_t)height, stride);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    assert(glGetError() == GL_NO_ERROR);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    uint8_t *rgb = g_malloc_n((size_t)width * height, 3);
    for (GLsizei row = 0; row < height; row++) {
        const uint8_t *source = rgba + (size_t)(height - row - 1) * stride;
        uint8_t *target = rgb + (size_t)row * width * 3;
        for (GLsizei column = 0; column < width; column++)
            memcpy(target + 3 * column, source + 4 * column, 3);
    }
    char *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, rgb, (gssize)width * height * 3);
    g_free(rgb);
    g_free(rgba);
    return digest;
}

void __real_glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void __wrap_glBlitFramebuffer(GLint sx, GLint sy, GLint end_x, GLint end_y,
    GLint dx, GLint dy, GLint dest_x, GLint dest_y, GLbitfield mask, GLenum filter)
{
    __real_glBlitFramebuffer(sx, sy, end_x, end_y, dx, dy, dest_x, dest_y, mask, filter);
    if (sequence < 0 || getenv("BOXRIG_VERIFY_GL_PRESENTATION") == NULL) return;
    GLint source, destination;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &source);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destination);
    if (destination != 0) return;
    GLsizei width = end_x - sx, height = end_y - sy;
    assert(source != 0 && width > 0 && height > 0 && dest_x - dx == width && dest_y - dy == height);
    char *retained = framebuffer_digest(sx, sy, width, height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    char *back = framebuffer_digest(dx, dy, width, height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)source);
    fprintf(report, "framebuffer\t%d\t%d\t%d\t%s\t%s\n", sequence, width, height, retained, back);
    g_free(back);
    g_free(retained);
}

EGLBoolean __real_eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean __wrap_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    uint64_t start = timestamp(CLOCK_THREAD_CPUTIME_ID);
    EGLBoolean result = __real_eglSwapBuffers(display, surface);
    add(PRESENT, start, 1);
    if (result == EGL_TRUE) submitted();
    return result;
}
#endif
