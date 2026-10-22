/* boxrig - Under MIT. */
#include "boxrig/gtk3.h"
#include "../../src/gtk/gl_surface.h"

#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static bool fail_context;
static bool fail_es_context;
static bool fail_es_renderer;
static bool fail_toggle_renderer;
static bool test_toggle_failure;
static unsigned int rejected_es_contexts;
static unsigned int rejected_es_renderers;
static unsigned int context_attempts;
static boxrig_terminal *resize_before_render;
static size_t accessible_rows_after_render;
/* Sum upload deltas across renderer instances so replacement cannot reset the observation. */
static size_t glyph_uploads;
static GtkGLArea *observed_area;
static GLint observed_alpha_bits = -1;
static boxrig_gl_target_format observed_format;
static boxrig_gl_alpha_mode observed_alpha_mode;

boxrig_ui_status __real_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *,
    boxrig_ui_view *, boxrig_composition *, boxrig_gtk_render_state *);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition, boxrig_gtk_render_state *state)
{
    boxrig_ogl_render_statistics before;
    boxrig_ogl_render_statistics after;
    CHECK(boxrig_gtk_gl_surface_get_statistics(surface, &before) == 0);
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_draw(surface, view, composition, state);
    CHECK(boxrig_gtk_gl_surface_get_statistics(surface, &after) == 0);
    CHECK(after.grayscale_glyph_upload_count >= before.grayscale_glyph_upload_count);
    CHECK(after.color_glyph_upload_count >= before.color_glyph_upload_count);
    glyph_uploads += after.grayscale_glyph_upload_count - before.grayscale_glyph_upload_count;
    glyph_uploads += after.color_glyph_upload_count - before.color_glyph_upload_count;
    return status;
}

void __real_gdk_gl_context_set_use_es(GdkGLContext *, int);
void __wrap_gdk_gl_context_set_use_es(GdkGLContext *context, int use_es)
{
    g_object_set_data(G_OBJECT(context), "test-request-es", GINT_TO_POINTER(use_es == 1));
    __real_gdk_gl_context_set_use_es(context, use_es);
}

gboolean __real_gdk_gl_context_realize(GdkGLContext *, GError **);
gboolean __wrap_gdk_gl_context_realize(GdkGLContext *context, GError **error)
{
    if (fail_es_context && g_object_get_data(G_OBJECT(context), "test-request-es") != NULL) {
        rejected_es_contexts++;
        g_set_error_literal(error, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
            "Injected ES context failure");
        return FALSE;
    }
    return __real_gdk_gl_context_realize(context, error);
}

boxrig_ui_status __real_boxrig_gtk_gl_surface_create(const boxrig_ogl_renderer_config *,
    boxrig_gtk_gl_surface **, boxrig_gtk_render_state *);
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_create(const boxrig_ogl_renderer_config *config,
    boxrig_gtk_gl_surface **surface, boxrig_gtk_render_state *state)
{
    if (fail_es_renderer && state->is_gles) {
        rejected_es_renderers++;
        *surface = NULL;
        state->status = BOXRIG_UI_UNAVAILABLE;
        state->reason = BOXRIG_GTK_RENDER_REASON_RESOURCE;
        return state->status;
    }
    if (fail_toggle_renderer && config->initial_target.alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED) {
        fail_toggle_renderer = false;
        *surface = NULL;
        state->status = BOXRIG_UI_UNAVAILABLE;
        state->reason = BOXRIG_GTK_RENDER_REASON_RESOURCE;
        return state->status;
    }
    observed_format = config->initial_target.format;
    observed_alpha_mode = config->initial_target.alpha_mode;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &observed_alpha_bits);
    CHECK(glGetError() == GL_NO_ERROR);
    return __real_boxrig_gtk_gl_surface_create(config, surface, state);
}

static gboolean before_render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    (void)area;
    (void)context;
    (void)data;
    if (resize_before_render != NULL) {
        size_t rows, columns;
        boxrig_terminal_size(resize_before_render, &rows, &columns);
        CHECK(boxrig_terminal_resize(resize_before_render, rows + 1, columns + 1) == BOXRIG_STATUS_OK);
        accessible_rows_after_render = rows + 1;
        resize_before_render = NULL;
    }
    return FALSE;
}

/** @copydoc gtk_gl_area_new */
GtkWidget *__real_gtk_gl_area_new(void);

static GdkGLContext *create_context(GtkGLArea *area, gpointer data)
{
    (void)data;
    context_attempts++;
    if (fail_context) {
        GError *error = g_error_new_literal(GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
            "Injected widget context failure");
        gtk_gl_area_set_error(area, error);
        g_error_free(error);
        g_signal_stop_emission_by_name(area, "create-context");
    }
    return NULL;
}

/** Instruments context creation without adding a product failure switch. */
GtkWidget *__wrap_gtk_gl_area_new(void)
{
    GtkWidget *area = __real_gtk_gl_area_new();
    observed_area = GTK_GL_AREA(area);
    g_signal_connect(area, "create-context", G_CALLBACK(create_context), NULL);
    g_signal_connect(area, "render", G_CALLBACK(before_render), NULL);
    return area;
}

typedef struct fixture {
    boxrig_terminal *terminal;
    BoxrigGtk3TerminalWidget *widget;
    uint64_t generation;
    uint64_t drawn_generation;
    int expected_width;
    int expected_height;
    bool done;
    bool failed;
} fixture;

static gboolean drawn(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)widget;
    (void)cr;
    fixture *test = data;
    test->drawn_generation = boxrig_gtk3_terminal_widget_presentation_generation(test->widget);
    if (accessible_rows_after_render != 0) {
        AtkText *text = ATK_TEXT(gtk_widget_get_accessible(GTK_WIDGET(test->widget)));
        char *contents = atk_text_get_text(text, 0, -1);
        size_t rows = 0;
        for (const char *p = contents; *p != '\0'; p++) rows += *p == '\n';
        CHECK(rows == accessible_rows_after_render);
        g_free(contents);
        accessible_rows_after_render = 0;
    }
    return FALSE;
}

static gboolean frame(gpointer data)
{
    fixture *test = data;
    boxrig_ui_layout_result layout;
    if (boxrig_gtk3_terminal_widget_get_layout(test->widget, &layout) != BOXRIG_UI_OK) return G_SOURCE_CONTINUE;
    boxrig_ui_component_frame_result result;
    boxrig_ui_status status = boxrig_gtk3_terminal_widget_frame(test->widget, &result);
    if (status != BOXRIG_UI_OK) {
        test->failed = true;
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    boxrig_ui_layout_action action;
    while (boxrig_gtk3_terminal_widget_take_layout_action(test->widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(test->terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk3_terminal_widget_complete_grid_resize(test->widget,
            action.revision, true) == BOXRIG_UI_OK);
    }
    boxrig_gtk_render_state state;
    CHECK(boxrig_gtk3_terminal_widget_get_render_state(test->widget, &state) == BOXRIG_UI_OK);
    if (state.active_backend != BOXRIG_GTK_RENDER_BACKEND_NONE &&
        boxrig_gtk3_terminal_widget_presentation_generation(test->widget) > test->generation &&
        test->drawn_generation == boxrig_gtk3_terminal_widget_presentation_generation(test->widget) &&
        (test->expected_width == 0 || gtk_widget_get_allocated_width(GTK_WIDGET(test->widget)) == test->expected_width) &&
        (test->expected_height == 0 || gtk_widget_get_allocated_height(GTK_WIDGET(test->widget)) == test->expected_height) &&
        !result.frame_pending &&
        !boxrig_ui_view_frame_requested(boxrig_gtk3_terminal_widget_view(test->widget))) {
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean deadline(gpointer data)
{
    (void)data;
    fprintf(stderr, "FAIL widget did not complete a frame\n");
    abort();
}

static void await_frame(fixture *test)
{
    test->done = false;
    g_idle_add(frame, test);
    guint timeout = g_timeout_add_seconds(30, deadline, NULL);
    while (!test->done) g_main_context_iteration(NULL, TRUE);
    CHECK(g_source_remove(timeout));
}

static cairo_surface_t *capture(GtkWidget *widget)
{
    int scale = gtk_widget_get_scale_factor(widget);
    int width = gtk_widget_get_allocated_width(widget) * scale;
    int height = gtk_widget_get_allocated_height(widget) * scale;
    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_surface_set_device_scale(image, scale, scale);
    cairo_t *cr = cairo_create(image);
    /* A host can paint its own background before drawing the terminal. */
    cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
    cairo_paint(cr);
    gtk_widget_draw(widget, cr);
    CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_flush(image);
    return image;
}

static void feed(fixture *test, const char *text)
{
    size_t consumed;
    CHECK(boxrig_terminal_feed(test->terminal, (const uint8_t *)text,
        strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(text));
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    CHECK(gtk_init_check(NULL, NULL));
    fail_context = strcmp(argv[2], "context") == 0;
    fail_es_context = strcmp(argv[2], "es-context") == 0;
    fail_es_renderer = strcmp(argv[2], "es-renderer") == 0;
    test_toggle_failure = strcmp(argv[2], "toggle") == 0;
    boxrig_gtk3_terminal_widget_config config = boxrig_gtk3_terminal_widget_config_default();
    config.accessibility_enabled = true;
    CHECK(config.render_policy == BOXRIG_GTK_RENDER_CAIRO);
    if (strcmp(argv[1], "prefer") == 0) config.render_policy = BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
    else if (strcmp(argv[1], "require") == 0) config.render_policy = BOXRIG_GTK_RENDER_OPENGL_REQUIRED;
    else CHECK(strcmp(argv[1], "cairo") == 0);
    fixture test = {0};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 4;
    terminal_config.columns = 40;
    CHECK(boxrig_terminal_create(&terminal_config, &test.terminal) == BOXRIG_STATUS_OK);
    boxrig_ui_model *model = NULL;
    CHECK(boxrig_ui_model_create(test.terminal, &model) == BOXRIG_UI_OK);
    boxrig_gtk_render_state state;
    CHECK(boxrig_gtk3_terminal_widget_create_with_render_state(model, &config,
        &test.widget, &state) == BOXRIG_UI_OK);
    CHECK(state.requested_policy == config.render_policy);
    CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
    g_signal_connect_after(test.widget, "draw", G_CALLBACK(drawn), &test);
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(test.widget, &metrics) == BOXRIG_FONT_CATALOG_OK);
    feed(&test, "\033[?25l\033[38;2;255;255;255;48;2;40;80;120m GTK terminal \033[0m\r\nlocal update");
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(window);
    gtk_widget_set_size_request(GTK_WIDGET(test.widget), 480, 160);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(test.widget));
    CHECK(boxrig_gtk3_terminal_widget_set_visible(test.widget, true) == BOXRIG_UI_OK);
    gtk_widget_show_all(window);
    await_frame(&test);
    CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
    fprintf(stderr, "OBSERVED GTK 3 policy=%s failure=%s backend=%d reason=%d status=%d es=%d message=%s\n",
        argv[1], argv[2], state.active_backend, state.reason, state.status, state.is_gles, state.message);
    bool unavailable = fail_context || fail_es_renderer;
    CHECK(test.failed == (unavailable && config.render_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED));
    if (config.render_policy != BOXRIG_GTK_RENDER_CAIRO) {
        if (fail_es_context) CHECK(rejected_es_contexts == 1);
        if (fail_es_renderer) CHECK(rejected_es_renderers == 1);
    }
    if (test.failed) {
        CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
        CHECK(state.reason == BOXRIG_GTK_RENDER_REASON_CONTEXT && state.status == BOXRIG_UI_UNAVAILABLE);
    } else {
        CHECK(state.active_backend == (config.render_policy == BOXRIG_GTK_RENDER_CAIRO || unavailable
            ? BOXRIG_GTK_RENDER_BACKEND_CAIRO : BOXRIG_GTK_RENDER_BACKEND_OPENGL));
        CHECK(context_attempts == (config.render_policy == BOXRIG_GTK_RENDER_CAIRO ? 0u : fail_es_renderer ? 2u : 1u));
        const char *expected_api = getenv("BOXRIG_TEST_GL_API");
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL && expected_api != NULL)
            CHECK(state.is_gles == (strcmp(expected_api, "gles") == 0));
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL) {
            CHECK(!gtk_gl_area_get_has_alpha(observed_area));
            CHECK(observed_alpha_bits == 0);
            CHECK(observed_format == BOXRIG_GL_TARGET_RGB8);
            CHECK(observed_alpha_mode == BOXRIG_GL_ALPHA_OPAQUE);
        }
        cairo_surface_t *before = capture(GTK_WIDGET(test.widget));
        uint32_t *before_pixels = (uint32_t *)cairo_image_surface_get_data(before);
        CHECK(before_pixels[0] == 0xff285078u);
        test.generation = boxrig_gtk3_terminal_widget_presentation_generation(test.widget);
        feed(&test, "\033[2;2HX");
        await_frame(&test);
        CHECK(!test.failed);
        cairo_surface_t *after = capture(GTK_WIDGET(test.widget));
        int stride = cairo_image_surface_get_stride(after);
        CHECK(stride == cairo_image_surface_get_stride(before));
        CHECK(memcmp(cairo_image_surface_get_data(before), cairo_image_surface_get_data(after),
            (size_t)stride * metrics.cell_height) == 0);
        CHECK(memcmp(cairo_image_surface_get_data(before) + stride * metrics.cell_height,
            cairo_image_surface_get_data(after) + stride * metrics.cell_height,
            (size_t)stride * metrics.cell_height) != 0);
        cairo_surface_destroy(before);
        cairo_surface_destroy(after);
        boxrig_font_effective_metrics after_metrics;
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(test.widget, &after_metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(metrics.cell_width == after_metrics.cell_width && metrics.cell_height == after_metrics.cell_height);
        CHECK(boxrig_gtk3_terminal_widget_model(test.widget) == model);
        uint64_t target_generation = state.target_generation;
        bool gl_before_resize = state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL;
        size_t uploads_before_resize = glyph_uploads;
        if (gl_before_resize) CHECK(uploads_before_resize > 0);
        test.generation = boxrig_gtk3_terminal_widget_presentation_generation(test.widget);
        test.expected_width = 640;
        test.expected_height = 240;
        gtk_window_resize(GTK_WINDOW(window), test.expected_width, test.expected_height);
        await_frame(&test);
        CHECK(!test.failed);
        CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.active_backend == (gl_before_resize
            ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO));
        if (gl_before_resize) {
            CHECK(state.target_generation > target_generation);
            CHECK(glyph_uploads == uploads_before_resize);
        }
        cairo_surface_t *resized = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(resized))[0] == 0xff285078u);
        CHECK(cairo_image_surface_get_width(resized) == test.expected_width * gtk_widget_get_scale_factor(GTK_WIDGET(test.widget)));
        cairo_surface_destroy(resized);

        boxrig_font_specification font;
        CHECK(boxrig_gtk3_terminal_widget_get_font_specification(test.widget, &font) == BOXRIG_UI_OK);
        font.size_dip *= 1.5f;
        target_generation = state.target_generation;
        test.generation = boxrig_gtk3_terminal_widget_presentation_generation(test.widget);
        CHECK(boxrig_gtk3_terminal_widget_set_font_specification(test.widget, &font) == BOXRIG_UI_OK);
        await_frame(&test);
        CHECK(!test.failed);
        CHECK(boxrig_gtk3_terminal_widget_get_effective_metrics(test.widget, &after_metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(after_metrics.cell_width > metrics.cell_width && after_metrics.cell_height > metrics.cell_height);
        CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.active_backend == (gl_before_resize
            ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO));
        if (gl_before_resize)
            CHECK(state.target_generation > target_generation);
        cairo_surface_t *enlarged = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(enlarged))[0] == 0xff285078u);
        cairo_surface_destroy(enlarged);
        boxrig_ui_transparency_config transparency = boxrig_ui_transparency_config_default();
        transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
        transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        transparency.background_opacity = 64;
        if (test_toggle_failure) {
            CHECK(config.render_policy != BOXRIG_GTK_RENDER_CAIRO);
            fail_toggle_renderer = true;
        }
        test.generation = boxrig_gtk3_terminal_widget_presentation_generation(test.widget);
        CHECK(boxrig_gtk3_terminal_widget_set_transparency(test.widget, &transparency) == BOXRIG_UI_OK);
        await_frame(&test);
        if (test_toggle_failure) {
            CHECK(!fail_toggle_renderer);
            CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
            if (config.render_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED) {
                CHECK(test.failed);
                CHECK(boxrig_ui_view_frame_requested(boxrig_gtk3_terminal_widget_view(test.widget)));
                CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
            } else {
                CHECK(!test.failed && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO);
                CHECK(!boxrig_ui_view_frame_requested(boxrig_gtk3_terminal_widget_view(test.widget)));
                cairo_surface_t *fallback = capture(GTK_WIDGET(test.widget));
                CHECK(((uint32_t *)cairo_image_surface_get_data(fallback))[0] == 0x400a141eu);
                cairo_surface_destroy(fallback);
            }
            goto teardown;
        }
        CHECK(!test.failed);
        CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.active_backend == (gl_before_resize
            ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO));
        if (gl_before_resize) {
            CHECK(gtk_gl_area_get_has_alpha(observed_area));
            CHECK(observed_alpha_bits == 8);
            CHECK(observed_format != BOXRIG_GL_TARGET_RGB8);
            CHECK(observed_alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED);
        }
        cairo_surface_t *alpha = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(alpha))[0] == 0x400a141eu);
        cairo_surface_destroy(alpha);
        transparency.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
        test.generation = boxrig_gtk3_terminal_widget_presentation_generation(test.widget);
        CHECK(boxrig_gtk3_terminal_widget_set_transparency(test.widget, &transparency) == BOXRIG_UI_OK);
        await_frame(&test);
        CHECK(!test.failed);
        CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.active_backend == (gl_before_resize
            ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO));
        if (gl_before_resize) {
            CHECK(!gtk_gl_area_get_has_alpha(observed_area));
            CHECK(observed_alpha_bits == 0);
            CHECK(observed_format == BOXRIG_GL_TARGET_RGB8);
            CHECK(observed_alpha_mode == BOXRIG_GL_ALPHA_OPAQUE);
        }
        cairo_surface_t *opaque = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(opaque))[0] == 0xff285078u);
        cairo_surface_destroy(opaque);
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL) {
            /* Complete a host resize between queued work and GTK rendering. */
            resize_before_render = test.terminal;
            test.generation = boxrig_gtk3_terminal_widget_presentation_generation(test.widget);
            feed(&test, "\033[2;2HZ");
            await_frame(&test);
            CHECK(resize_before_render == NULL && !test.failed);
            CHECK(boxrig_gtk3_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
            CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL && state.status == BOXRIG_UI_OK);
        }
    }
teardown:
    gtk_widget_destroy(window);
    g_object_unref(window);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(test.terminal);
    printf("PASS GTK 3 widget policy=%s failure=%s backend=%d reason=%d\n",
        argv[1], argv[2], state.active_backend, state.reason);
    return 0;
}
