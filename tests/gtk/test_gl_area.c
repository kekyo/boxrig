/* boxrig - Under MIT. */
#include "gtk/gl_area.h"
#include "pangocairo/renderer.h"

#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

typedef struct driver {
    GtkWidget *window;
    GtkWidget *stack;
    GtkWidget *area;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_pango_cairo_renderer *cairo;
    boxrig_ogl_renderer *gl;
    boxrig_gtk_render_state state;
    const char *failure;
    const char *image_path;
    unsigned int context_attempts;
    unsigned int frames;
    bool finished;
    bool failed;
} driver;

static void verify_image(driver *app, cairo_surface_t *image)
{
    cairo_surface_flush(image);
    const uint32_t *pixels = (const uint32_t *)cairo_image_surface_get_data(image);
    int stride = cairo_image_surface_get_stride(image) / 4;
    int width = cairo_image_surface_get_width(image);
    int height = cairo_image_surface_get_height(image);
    if (app->image_path != NULL) {
        CHECK(cairo_surface_write_to_png(image, app->image_path) == CAIRO_STATUS_SUCCESS);
    }
    size_t colored = 0, text = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = pixels[y * stride + x];
            CHECK((pixel >> 24) == 255);
            if ((pixel & 0xffffffu) == 0x285078u) colored++;
            /* Glyph coverage need not contain a fully covered white pixel. */
            if (((pixel >> 16) & 255u) > 180 &&
                ((pixel >> 8) & 255u) > 180 && (pixel & 255u) > 180) text++;
        }
    }
    CHECK(colored > 100);
    CHECK(text > 10);
}

static cairo_surface_t *capture_gl(unsigned int width, unsigned int height)
{
    uint8_t *rgba = malloc((size_t)width * height * 4);
    CHECK(rgba != NULL);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    /* Readback belongs to the test observer, never to normal rendering. */
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    CHECK(glGetError() == GL_NO_ERROR);
    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        (int)width, (int)height);
    CHECK(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(image);
    int stride = cairo_image_surface_get_stride(image) / 4;
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            const uint8_t *p = rgba + ((size_t)(height - y - 1) * width + x) * 4;
            pixels[y * (size_t)stride + x] = ((uint32_t)p[3] << 24) |
                ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    cairo_surface_mark_dirty(image);
    free(rgba);
    return image;
}

static void draw_cairo(driver *app, cairo_t *cr, int width, int height)
{
    int scale = gtk_widget_get_scale_factor(app->stack);
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(app->cairo,
        (unsigned int)(width * scale), (unsigned int)(height * scale)) == 0);
    CHECK(boxrig_pango_cairo_renderer_draw_view(app->cairo, app->view) == 0);
    cairo_surface_t *image = boxrig_pango_cairo_renderer_surface(app->cairo);
    verify_image(app, image);
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_paint(cr);
    app->state.active_backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
    app->frames++;
    app->finished = true;
}

#if GTK_MAJOR_VERSION == 3
static gboolean cairo_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    draw_cairo(data, cr, gtk_widget_get_allocated_width(widget),
        gtk_widget_get_allocated_height(widget));
    return TRUE;
}
#else
static void cairo_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
    gpointer data)
{
    (void)area;
    draw_cairo(data, cr, width, height);
}
#endif

static gboolean apply_failure(gpointer data)
{
    driver *app = data;
    if (app->state.requested_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED) {
        app->failed = true;
        app->finished = true;
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "cairo");
    }
    return G_SOURCE_REMOVE;
}

static GdkGLContext *create_context(GtkGLArea *area, gpointer data)
{
    driver *app = data;
    app->context_attempts++;
    if (strcmp(app->failure, "context") == 0) {
        GError *error = g_error_new_literal(GDK_GL_ERROR,
            GDK_GL_ERROR_NOT_AVAILABLE, "Injected context creation failure");
        gtk_gl_area_set_error(area, error);
        g_error_free(error);
        g_signal_stop_emission_by_name(area, "create-context");
    }
    return NULL;
}

static driver *creating_driver;
GtkWidget *__real_gtk_gl_area_new(void);
GtkWidget *__wrap_gtk_gl_area_new(void)
{
    GtkWidget *area = __real_gtk_gl_area_new();
    /* Observe/inject before the bridge's create-context handler runs. */
    g_signal_connect(area, "create-context", G_CALLBACK(create_context), creating_driver);
    return area;
}

static void realized(GtkGLArea *area, gpointer data)
{
    driver *app = data;
    gtk_gl_area_make_current(area);
    const GError *error = gtk_gl_area_get_error(area);
    if (error != NULL) {
        boxrig_gtk_gl_area_record_error(&app->state,
            BOXRIG_GTK_RENDER_REASON_CONTEXT, BOXRIG_UI_UNAVAILABLE,
            error->message);
        g_idle_add(apply_failure, app);
    }
}

static boxrig_gl_function missing_capability(void *context, const char *name)
{
    if (strcmp(name, "glTexImage2D") == 0) return NULL;
    return boxrig_gtk_gl_area_resolve(context, name);
}

static gboolean render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    driver *app = data;
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    config.rendering_policy.output_scale = (float)
        gtk_widget_get_scale_factor(GTK_WIDGET(area));
    (void)context;
    boxrig_gtk_gl_destination destination;
    CHECK(boxrig_gtk_gl_area_describe(area, 1, app->frames + 1, &config.runtime,
        &destination, &app->state) == BOXRIG_UI_OK);
    /* This driver deliberately renders directly into a verified RGBA target.
     * Product widgets use a separate retained surface instead. */
    CHECK(destination.alpha_bits == 8);
    config.initial_target = (boxrig_gl_target){destination.draw_framebuffer,
        destination.width, destination.height, destination.generation,
        destination.srgb ? BOXRIG_GL_TARGET_SRGB8_ALPHA8 : BOXRIG_GL_TARGET_RGBA8,
        BOXRIG_GL_ALPHA_PREMULTIPLIED, BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT, 0};
    CHECK(config.initial_target.draw_framebuffer != 0);
    CHECK(config.initial_target.samples == 0);
    CHECK(app->state.is_gles == !epoxy_is_desktop_gl());
    const char *expected_api = getenv("BOXRIG_TEST_GL_API");
    if (expected_api != NULL && strcmp(expected_api, "default") != 0) {
        CHECK(strcmp(expected_api, "gl") == 0 || strcmp(expected_api, "gles") == 0);
        CHECK(app->state.is_gles == (strcmp(expected_api, "gles") == 0));
    }
#if GTK_MAJOR_VERSION == 3
    int logical_width = gtk_widget_get_allocated_width(GTK_WIDGET(area));
#else
    int logical_width = gtk_widget_get_width(GTK_WIDGET(area));
#endif
    CHECK(config.initial_target.width == (unsigned int)
        (logical_width * gtk_widget_get_scale_factor(GTK_WIDGET(area))));
    boxrig_gl_diagnostic diagnostic = {0};
    boxrig_gl_status status;
    if (app->gl == NULL) {
        if (strcmp(app->failure, "capability") == 0) {
            config.runtime.resolve = missing_capability;
        }
        status = boxrig_ogl_renderer_create(&config, &app->gl, &diagnostic);
    } else {
        status = boxrig_ogl_renderer_set_target(app->gl, 1,
            &config.initial_target, &diagnostic);
    }
    if (status != BOXRIG_GL_OK) {
        boxrig_gtk_gl_area_record_diagnostic(&app->state, &diagnostic);
        g_idle_add(apply_failure, app);
        return TRUE;
    }
    CHECK(boxrig_ogl_renderer_cell_width(app->gl) ==
        boxrig_pango_cairo_renderer_cell_width(app->cairo));
    CHECK(boxrig_ogl_renderer_cell_height(app->gl) ==
        boxrig_pango_cairo_renderer_cell_height(app->cairo));
    /* GtkGLArea may rotate targets: the initial driver deliberately redraws all. */
    boxrig_ui_dirty all = {0, 4, 0, 40};
    CHECK(boxrig_ui_view_invalidate(app->view, BOXRIG_UI_DIRTY_PRESENT, all) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(app->gl, app->view) == 0);
    cairo_surface_t *image = capture_gl(config.initial_target.width, config.initial_target.height);
    verify_image(app, image);
    cairo_surface_destroy(image);
    boxrig_ogl_render_statistics statistics;
    CHECK(boxrig_ogl_renderer_get_render_statistics(app->gl, &statistics) == 0);
    CHECK(statistics.full_frame_software_upload_count == 0 &&
        statistics.readback_count == 0 && statistics.swap_count == 0 &&
        statistics.finish_count == 0 && statistics.wait_count == 0);
    printf("GL vendor=%s renderer=%s version=%s target=%ux%u fbo=%u format=%d alpha=%d\n",
        glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
        config.initial_target.width, config.initial_target.height,
        config.initial_target.draw_framebuffer, config.initial_target.format,
        config.initial_target.alpha_mode);
    app->state.active_backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
    app->frames++;
    app->finished = true;
    return TRUE;
}

static void unrealized(GtkGLArea *area, gpointer data)
{
    driver *app = data;
    if (app->gl != NULL) {
        gtk_gl_area_make_current(area);
        CHECK(boxrig_ogl_renderer_destroy(&app->gl, 1, NULL) == BOXRIG_GL_OK);
    }
}

static gboolean deadline(gpointer data)
{
    driver *app = data;
    fprintf(stderr, "FAIL GTK %d driver did not finish a frame\n", GTK_MAJOR_VERSION);
    app->failed = true;
    app->finished = true;
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3 && argc <= 4);
    driver app = {0};
    if (strcmp(argv[1], "cairo") == 0) app.state.requested_policy = BOXRIG_GTK_RENDER_CAIRO;
    else if (strcmp(argv[1], "prefer") == 0) app.state.requested_policy = BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
    else if (strcmp(argv[1], "require") == 0) app.state.requested_policy = BOXRIG_GTK_RENDER_OPENGL_REQUIRED;
    else return 1;
    app.failure = argv[2];
    CHECK(strcmp(app.failure, "none") == 0 || strcmp(app.failure, "context") == 0 ||
        strcmp(app.failure, "capability") == 0 || strcmp(app.failure, "unavailable") == 0);
    app.image_path = argc == 4 ? argv[3] : NULL;
#if GTK_MAJOR_VERSION == 3
    CHECK(gtk_init_check(NULL, NULL));
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    CHECK(gtk_init_check());
    app.window = gtk_window_new();
#endif
    g_object_ref_sink(app.window);
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 4;
    terminal_config.columns = 40;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&terminal_config, &app.terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(app.terminal, &app.model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(app.model, &view_config, &app.view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(480, 160, &app.cairo) == 0);
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_rendering_policy font_policy = boxrig_font_rendering_policy_default();
    font_policy.output_scale = (float)gtk_widget_get_scale_factor(app.window);
    CHECK(boxrig_pango_cairo_renderer_set_font(app.cairo, &font, &font_policy) == BOXRIG_FONT_CATALOG_OK);
    const char text[] = "\033[?25l\033[38;2;255;255;255;48;2;40;80;120m GTK OpenGL / Cairo "
        "\033[0m\r\nboxrig terminal driver";
    size_t consumed;
    CHECK(boxrig_terminal_feed(app.terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(text) - 1);
    CHECK(boxrig_ui_model_synchronize(app.model) == BOXRIG_UI_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app.stack = gtk_stack_new();
    gtk_widget_set_size_request(app.stack, 480, 160);
    GtkWidget *cairo = gtk_drawing_area_new();
    GtkWidget *label = gtk_label_new("boxrig GtkGLArea driver");
#if GTK_MAJOR_VERSION == 3
    g_signal_connect(cairo, "draw", G_CALLBACK(cairo_draw), &app);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(app.window), box);
#else
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(cairo), cairo_draw, &app, NULL);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), app.stack);
    gtk_window_set_child(GTK_WINDOW(app.window), box);
#endif
    gtk_stack_add_named(GTK_STACK(app.stack), cairo, "cairo");
    if (app.state.requested_policy != BOXRIG_GTK_RENDER_CAIRO) {
        creating_driver = &app;
        app.area = boxrig_gtk_gl_area_new();
        creating_driver = NULL;
        g_signal_connect(app.area, "realize", G_CALLBACK(realized), &app);
        g_signal_connect(app.area, "render", G_CALLBACK(render), &app);
        g_signal_connect(app.area, "unrealize", G_CALLBACK(unrealized), &app);
        gtk_stack_add_named(GTK_STACK(app.stack), app.area, "gl");
#if GTK_MAJOR_VERSION == 3
        gtk_widget_show(app.area);
#endif
        gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "gl");
    }
#if GTK_MAJOR_VERSION == 3
    gtk_widget_show_all(app.window);
#else
    gtk_window_present(GTK_WINDOW(app.window));
#endif
    guint timeout = g_timeout_add_seconds(30, deadline, &app);
    while (!app.finished) g_main_context_iteration(NULL, TRUE);
    CHECK(g_source_remove(timeout));
    fprintf(stderr, "OBSERVED GTK %d policy=%s failure=%s backend=%d reason=%d status=%d frames=%u message=%s\n",
        GTK_MAJOR_VERSION, argv[1], argv[2], app.state.active_backend,
        app.state.reason, app.state.status, app.frames, app.state.message);
    bool expected_failure = app.state.requested_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED &&
        strcmp(app.failure, "none") != 0;
    CHECK(app.failed == expected_failure);
    if (app.state.requested_policy == BOXRIG_GTK_RENDER_CAIRO) {
        CHECK(app.context_attempts == 0);
        CHECK(app.state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    } else if (strcmp(app.failure, "none") == 0) {
        CHECK(app.context_attempts == 1);
        CHECK(app.state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    } else {
        CHECK(app.state.status == BOXRIG_UI_UNAVAILABLE);
        CHECK(app.state.reason == (strcmp(app.failure, "capability") == 0
            ? BOXRIG_GTK_RENDER_REASON_CAPABILITY : BOXRIG_GTK_RENDER_REASON_CONTEXT));
        CHECK(app.state.message[0] != '\0');
        CHECK(app.state.active_backend == (expected_failure
            ? BOXRIG_GTK_RENDER_BACKEND_NONE : BOXRIG_GTK_RENDER_BACKEND_CAIRO));
    }
    printf("PASS GTK %d policy=%s failure=%s backend=%d reason=%d status=%d frames=%u attempts=%u message=%s\n",
        GTK_MAJOR_VERSION, argv[1], argv[2], app.state.active_backend,
        app.state.reason, app.state.status, app.frames, app.context_attempts, app.state.message);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(app.window);
#else
    gtk_window_destroy(GTK_WINDOW(app.window));
#endif
    CHECK(app.gl == NULL);
    g_object_unref(app.window);
    boxrig_pango_cairo_renderer_destroy(app.cairo);
    boxrig_ui_view_destroy(app.view);
    boxrig_ui_model_destroy(app.model);
    boxrig_terminal_destroy(app.terminal);
    return 0;
}
