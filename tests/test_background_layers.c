/* boxrig - Under MIT. */
#define GL_GLEXT_PROTOTYPES 1
#include "boxrig/ui.h"
#include "boxrig/ui/pixel.h"
#include "boxrig/render/opengl.h"
#include "pangocairo/renderer.h"
#include "opengl_host.h"
#include "boxrig/x11/renderer.h"
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 320
#define HEIGHT 120
typedef struct renderer {
    boxrig_pango_cairo_renderer *cairo;
    opengl_host *host;
    boxrig_ogl_renderer *gl;
    boxrig_x11_renderer *x11;
    Display *display;
    Window window;
    Colormap colormap;
    uint32_t framebuffer, texture;
    uint64_t generation;
} renderer;

static void create(renderer *r, int backend)
{
    memset(r, 0, sizeof(*r));
    if (backend == 2) {
        r->display = XOpenDisplay(NULL);
        assert(r->display != NULL);
        int screen = DefaultScreen(r->display);
        XVisualInfo visual;
        assert(XMatchVisualInfo(r->display, screen, 32, TrueColor, &visual));
        r->colormap = XCreateColormap(r->display, RootWindow(r->display, screen), visual.visual, AllocNone);
        XSetWindowAttributes attributes = {0};
        attributes.colormap = r->colormap;
        r->window = XCreateWindow(r->display, RootWindow(r->display, screen), 0, 0, WIDTH, HEIGHT,
            0, 32, InputOutput, visual.visual, CWColormap | CWBorderPixel | CWBackPixel, &attributes);
        XMapWindow(r->display, r->window);
        XSync(r->display, False);
        assert(boxrig_x11_renderer_create(r->display, r->window, &r->x11) == 0);
        return;
    }
    if (backend == 0) {
        assert(boxrig_pango_cairo_renderer_create(WIDTH, HEIGHT, &r->cairo) == 0);
        return;
    }
    r->generation = 1;
    opengl_host_report report;
    assert(opengl_host_create("egl", &r->host, &report));
    r->framebuffer = opengl_host_create_framebuffer(r->host,
        BOXRIG_GL_TARGET_RGBA8, WIDTH, HEIGHT, &r->texture);
    assert(r->framebuffer != 0);
    boxrig_ogl_renderer_config c = boxrig_ogl_renderer_config_default();
    c.runtime.resolve = opengl_host_resolve;
    c.runtime.resolver_context = r->host;
    c.runtime.query_current_context = opengl_host_query_current;
    c.runtime.current_context_query_context = r->host;
    c.runtime.context_generation = c.runtime.target_generation = 1;
    c.initial_target = (boxrig_gl_target){.draw_framebuffer = r->framebuffer,
        .width = WIDTH, .height = HEIGHT, .generation = 1,
        .format = BOXRIG_GL_TARGET_RGBA8, .alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED,
        .origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT};
    assert(boxrig_ogl_renderer_create(&c, &r->gl, NULL) == BOXRIG_GL_OK);
}

static void resize(renderer *r, unsigned int width, unsigned int height)
{
    if (r->x11 != NULL) {
        XResizeWindow(r->display, r->window, width, height);
        assert(boxrig_x11_renderer_set_surface_size(r->x11, width, height) == 0);
    } else if (r->cairo != NULL) {
        assert(boxrig_pango_cairo_renderer_set_surface_size(r->cairo, (int)width, (int)height) == 0);
    } else {
        uint32_t texture, framebuffer = opengl_host_create_framebuffer(r->host,
            BOXRIG_GL_TARGET_RGBA8, width, height, &texture);
        assert(framebuffer != 0);
        boxrig_gl_target target = {.draw_framebuffer = framebuffer, .width = width, .height = height,
            .generation = ++r->generation, .format = BOXRIG_GL_TARGET_RGBA8,
            .alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED, .origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT};
        assert(boxrig_ogl_renderer_set_target(r->gl, 1, &target, NULL) == BOXRIG_GL_OK);
        opengl_host_delete_framebuffer(r->host, r->framebuffer, r->texture);
        r->framebuffer = framebuffer; r->texture = texture;
    }
}

static uint32_t *draw(renderer *r, boxrig_ui_view *view)
{
    uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
    assert(pixels != NULL);
    if (r->x11 != NULL) {
        assert(boxrig_x11_renderer_draw_view(r->x11, view) == 0);
        XSync(r->display, False);
        XImage *image = XGetImage(r->display, r->window, 0, 0, WIDTH, HEIGHT, AllPlanes, ZPixmap);
        assert(image != NULL);
        for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++)
            pixels[y * WIDTH + x] = (uint32_t)XGetPixel(image, x, y);
        XDestroyImage(image);
    } else if (r->cairo != NULL) {
        assert(boxrig_pango_cairo_renderer_draw_view(r->cairo, view) == 0);
        cairo_surface_t *s = boxrig_pango_cairo_renderer_surface(r->cairo);
        cairo_surface_flush(s);
        const unsigned char *bytes = cairo_image_surface_get_data(s);
        int stride = cairo_image_surface_get_stride(s);
        for (int y = 0; y < HEIGHT; y++) memcpy(pixels + y * WIDTH, bytes + y * stride, WIDTH * 4);
    } else {
        assert(boxrig_ogl_renderer_draw_view(r->gl, view) == 0);
        unsigned char rgba[WIDTH * HEIGHT * 4];
        glBindFramebuffer(GL_READ_FRAMEBUFFER, r->framebuffer);
        glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        assert(glGetError() == GL_NO_ERROR);
        for (size_t y = 0; y < HEIGHT; y++) for (size_t x = 0; x < WIDTH; x++) {
            const unsigned char *p = rgba + ((HEIGHT - y - 1) * WIDTH + x) * 4;
            pixels[y * WIDTH + x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                ((uint32_t)p[1] << 8) | p[2];
        }
    }
    return pixels;
}

static void compare(const uint32_t *actual, const uint32_t *terminal, uint32_t layer)
{
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        uint32_t expected = boxrig_ui_argb32_source_over(terminal[i], layer);
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            int difference = (int)((actual[i] >> shift) & 255) - (int)((expected >> shift) & 255);
            if (abs(difference) > 1) {
                fprintf(stderr, "pixel %zu: actual %08x expected %08x terminal %08x layer %08x\n",
                    i, actual[i], expected, terminal[i], layer);
                abort();
            }
        }
    }
}

static void feed(boxrig_terminal *terminal, boxrig_ui_model *model, const char *text)
{
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(text));
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
}

static void test(int backend)
{
    renderer r;
    create(&r, backend);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 3; tc.columns = 12;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    vc.blink.cursor_enabled = vc.blink.text_enabled = false;
    assert(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    feed(terminal, model, "\033[48;2;64;128;192mHi\033_Ga=T,f=32,s=1,v=1;AP8AgA==\033\\");
    const uint8_t alphas[] = {0, 128, 255};
    for (size_t opacity = 0; opacity < 3; opacity++) {
        boxrig_ui_transparency_state state = {0};
        state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        state.effective_background_opacity = alphas[opacity];
        state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
        if (r.x11 != NULL) assert(boxrig_x11_renderer_apply_transparency_state(r.x11, &state) == 0);
        else if (r.gl != NULL) assert(boxrig_ogl_renderer_apply_transparency_state(r.gl, &state) == 0);
        else assert(boxrig_pango_cairo_renderer_apply_transparency_state(r.cairo, &state) == 0);
        assert(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0}) == BOXRIG_UI_OK);
        uint32_t *baseline = draw(&r, view);
        for (size_t alpha = 0; alpha < 3; alpha++) {
            boxrig_ui_rgba color = {255, 0, 0, alphas[alpha]};
            assert(boxrig_ui_view_set_background_color(view, color) == BOXRIG_UI_OK);
            uint32_t *actual = draw(&r, view);
            compare(actual, baseline, boxrig_ui_argb32_premultiply(255, 0, 0, alphas[alpha]));
            uint32_t *again = draw(&r, view);
            assert(memcmp(actual, again, WIDTH * HEIGHT * 4) == 0);
            free(again); free(actual);
        }
        free(baseline);
    }
    boxrig_ui_transparency_state partial_state = {0};
    partial_state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    partial_state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    partial_state.effective_background_opacity = 128;
    if (r.x11 != NULL) assert(boxrig_x11_renderer_apply_transparency_state(r.x11, &partial_state) == 0);
    else if (r.gl != NULL) assert(boxrig_ogl_renderer_apply_transparency_state(r.gl, &partial_state) == 0);
    else assert(boxrig_pango_cairo_renderer_apply_transparency_state(r.cairo, &partial_state) == 0);
    assert(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){255, 0, 0, 128}) == BOXRIG_UI_OK);
    free(draw(&r, view));
    feed(terminal, model, "\033[2;2HX");
    uint32_t *partial = draw(&r, view);
    assert(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0}) == BOXRIG_UI_OK);
    uint32_t *baseline = draw(&r, view);
    compare(partial, baseline, UINT32_C(0x80800000));
    free(partial); free(baseline);
    /* Application pixels are copied, bounded, and independent of Kitty. */
    uint8_t rgba[] = {0, 255, 0, 128};
    boxrig_ui_background_image image = {1, 1, 4, sizeof(rgba), rgba, 128, BOXRIG_UI_BACKGROUND_COVER};
    assert(boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_OK);
    rgba[1] = 0;
    assert(boxrig_ui_view_get_background_image(view).pixels[0] == UINT32_C(0x40004000));
    boxrig_ui_view_config bounded = boxrig_ui_view_config_default();
    bounded.maximum_background_image_bytes = 3;
    boxrig_ui_view *limited;
    assert(boxrig_ui_view_create(model, &bounded, &limited) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_background_image(limited, &image) == BOXRIG_UI_LIMIT_EXCEEDED);
    assert(boxrig_ui_view_get_background_image(limited).pixels == NULL);
    boxrig_ui_view_destroy(limited);
    boxrig_ui_background_image_view snapshot = boxrig_ui_view_get_background_image(view);
    snapshot.width = 2;
    boxrig_ui_background_rectangle place = boxrig_ui_background_image_place(snapshot, 100, 80);
    assert(place.x == -30 && place.y == 0 && place.width == 160 && place.height == 80);
    snapshot.fit = BOXRIG_UI_BACKGROUND_CONTAIN;
    place = boxrig_ui_background_image_place(snapshot, 100, 80);
    assert(place.x == 0 && place.y == 15 && place.width == 100 && place.height == 50);
    snapshot.fit = BOXRIG_UI_BACKGROUND_STRETCH;
    place = boxrig_ui_background_image_place(snapshot, 100, 80);
    assert(place.x == 0 && place.y == 0 && place.width == 100 && place.height == 80);
    image.stride = 3;
    assert(boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_view_get_background_image(view).pixels[0] == UINT32_C(0x40004000));
    free(draw(&r, view));
    feed(terminal, model, "\033_Ga=d,d=A\033\\\033[3;1H\nscroll\n");
    partial = draw(&r, view);
    uint32_t *unchanged = draw(&r, view);
    assert(memcmp(partial, unchanged, WIDTH * HEIGHT * 4) == 0);
    free(unchanged);
    assert(boxrig_ui_view_set_background_image(view, NULL) == BOXRIG_UI_OK);
    baseline = draw(&r, view);
    compare(partial, baseline, UINT32_C(0x40004000));
    free(partial); free(baseline);
    image.stride = 4;
    rgba[1] = 255;
    assert(boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_OK);
    resize(&r, WIDTH * 2, HEIGHT * 2);
    partial = draw(&r, view);
    assert(boxrig_ui_view_set_background_image(view, NULL) == BOXRIG_UI_OK);
    baseline = draw(&r, view);
    compare(partial, baseline, UINT32_C(0x40004000));
    free(partial); free(baseline);
    resize(&r, WIDTH, HEIGHT);
    /* Contain leaves centered transparent margins, including non-cell viewport pixels. */
    rgba[1] = 255;
    image.stride = 4;
    image.fit = BOXRIG_UI_BACKGROUND_CONTAIN;
    assert(boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_OK);
    partial = draw(&r, view);
    assert(boxrig_ui_view_set_background_image(view, NULL) == BOXRIG_UI_OK);
    baseline = draw(&r, view);
    for (size_t y = 0; y < HEIGHT; y++) for (size_t x = 0; x < WIDTH; x++) {
        uint32_t layer = x >= (WIDTH - HEIGHT) / 2 && x < (WIDTH + HEIGHT) / 2 ? UINT32_C(0x40004000) : 0;
        uint32_t expected = boxrig_ui_argb32_source_over(baseline[y * WIDTH + x], layer);
        for (unsigned int shift = 0; shift < 32; shift += 8)
            assert(abs((int)((partial[y * WIDTH + x] >> shift) & 255) - (int)((expected >> shift) & 255)) <= 1);
    }
    free(partial); free(baseline);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (r.x11 != NULL) {
        boxrig_x11_renderer_destroy(r.x11);
        XDestroyWindow(r.display, r.window);
        XFreeColormap(r.display, r.colormap);
        XCloseDisplay(r.display);
    } else if (r.cairo != NULL) boxrig_pango_cairo_renderer_destroy(r.cairo);
    else {
        assert(boxrig_ogl_renderer_destroy(&r.gl, 1, NULL) == BOXRIG_GL_OK);
        opengl_host_delete_framebuffer(r.host, r.framebuffer, r.texture);
        opengl_host_destroy(r.host);
    }
    printf("PASS %s RGBA background behind terminal pixels\n", backend == 2 ? "X11" : backend == 1 ? "OpenGL" : "Cairo");
}

int main(void)
{
    test(0);
    test(1);
    test(2);
    cairo_debug_reset_static_data();
    FcFini();
    return 0;
}
