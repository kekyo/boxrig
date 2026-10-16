/* boxrig - Under MIT. */
#include "boxrig/gtk4.h"
#include <epoxy/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static bool fail_context;
static unsigned int context_attempts;
static GLint targets[64];
static size_t target_count;
static GskRenderNode *retained_snapshots[8];
static size_t retained_count;
static boxrig_terminal *resize_before_render;
static size_t accessible_rows_after_render;

static gboolean observe_target(GtkGLArea *area, GdkGLContext *context, gpointer data)
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
    GLint texture = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texture);
    CHECK(texture != 0 && glGetError() == GL_NO_ERROR);
    for (size_t index = 0; index < target_count; index++)
        if (targets[index] == texture) return FALSE;
    CHECK(target_count < G_N_ELEMENTS(targets));
    targets[target_count++] = texture;
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
    g_signal_connect(area, "create-context", G_CALLBACK(create_context), NULL);
    g_signal_connect(area, "render", G_CALLBACK(observe_target), NULL);
    return area;
}

typedef struct fixture {
    boxrig_terminal *terminal;
    BoxrigGtk4TerminalWidget *widget;
    uint64_t generation;
    uint64_t drawn_generation;
    int expected_width;
    int expected_height;
    unsigned int phase;
    bool done;
    bool failed;
} fixture;

static void drawn(GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    fixture *test = data;
    test->drawn_generation = boxrig_gtk4_terminal_widget_presentation_generation(test->widget);
    if (accessible_rows_after_render != 0) {
        GtkAccessibleText *text = GTK_ACCESSIBLE_TEXT(test->widget);
        GBytes *contents = GTK_ACCESSIBLE_TEXT_GET_IFACE(text)->get_contents(text, 0, G_MAXUINT);
        gsize length;
        const char *bytes = g_bytes_get_data(contents, &length);
        size_t rows = 0;
        for (gsize i = 0; i < length; i++) rows += bytes[i] == '\n';
        CHECK(rows == accessible_rows_after_render);
        g_bytes_unref(contents);
        accessible_rows_after_render = 0;
    }
}

static gboolean frame(gpointer data)
{
    fixture *test = data;
    boxrig_ui_layout_result layout;
    if (boxrig_gtk4_terminal_widget_get_layout(test->widget, &layout) != BOXRIG_UI_OK) return G_SOURCE_CONTINUE;
    boxrig_ui_component_frame_result result;
    boxrig_ui_status status = boxrig_gtk4_terminal_widget_frame(test->widget, &result);
    if (status != BOXRIG_UI_OK) {
        boxrig_gtk_render_state rendering;
        CHECK(boxrig_gtk4_terminal_widget_get_render_state(test->widget, &rendering) == BOXRIG_UI_OK);
        fprintf(stderr, "GTK frame status=%d reason=%d message=%s\n", status, rendering.reason, rendering.message);
        test->failed = true;
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    boxrig_ui_layout_action action;
    while (boxrig_gtk4_terminal_widget_take_layout_action(test->widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(test->terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_gtk4_terminal_widget_complete_grid_resize(test->widget,
            action.revision, true) == BOXRIG_UI_OK);
    }
    boxrig_gtk_render_state state;
    CHECK(boxrig_gtk4_terminal_widget_get_render_state(test->widget, &state) == BOXRIG_UI_OK);
    if (state.active_backend != BOXRIG_GTK_RENDER_BACKEND_NONE &&
        boxrig_gtk4_terminal_widget_presentation_generation(test->widget) > test->generation &&
        test->drawn_generation == boxrig_gtk4_terminal_widget_presentation_generation(test->widget) &&
        (test->expected_width == 0 || gtk_widget_get_width(GTK_WIDGET(test->widget)) == test->expected_width) &&
        (test->expected_height == 0 || gtk_widget_get_height(GTK_WIDGET(test->widget)) == test->expected_height) &&
        !result.frame_pending &&
        !boxrig_ui_view_frame_requested(boxrig_gtk4_terminal_widget_view(test->widget))) {
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean deadline(gpointer data)
{
    fixture *test = data;
    fprintf(stderr, "FAIL phase=%u generation=%llu current=%llu painted=%llu dirty=%d\n",
        test->phase, (unsigned long long)test->generation,
        (unsigned long long)boxrig_gtk4_terminal_widget_presentation_generation(test->widget),
        (unsigned long long)test->drawn_generation,
        boxrig_ui_view_frame_requested(boxrig_gtk4_terminal_widget_view(test->widget)));
    abort();
}

static void await_frame(fixture *test)
{
    test->done = false;
    test->phase++;
    g_idle_add(frame, test);
    guint timeout = g_timeout_add_seconds(30, deadline, test);
    while (!test->done) g_main_context_iteration(NULL, TRUE);
    CHECK(g_source_remove(timeout));
}

static cairo_surface_t *capture(GtkWidget *widget)
{
    int scale = gtk_widget_get_scale_factor(widget);
    int width = gtk_widget_get_width(widget) * scale;
    int height = gtk_widget_get_height(widget) * scale;
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gtk_widget_snapshot_child(gtk_widget_get_parent(widget), widget, snapshot);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    /* Keep GTK's previous textures alive, as a compositor can do while a new
     * terminal frame is prepared. The next GLArea render must rotate them. */
    if (retained_count < G_N_ELEMENTS(retained_snapshots))
        retained_snapshots[retained_count++] = gsk_render_node_ref(node);
    snapshot = gtk_snapshot_new();
    gtk_snapshot_scale(snapshot, scale, scale);
    gtk_snapshot_append_node(snapshot, node);
    gsk_render_node_unref(node);
    node = gtk_snapshot_free_to_node(snapshot);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    GskRenderer *renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &bounds);
    CHECK(texture != NULL);
    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    GdkTextureDownloader *download = gdk_texture_downloader_new(texture);
    gdk_texture_downloader_set_format(download, GDK_MEMORY_DEFAULT);
    gdk_texture_downloader_download_into(download, cairo_image_surface_get_data(image),
        cairo_image_surface_get_stride(image));
    cairo_surface_mark_dirty(image);
    gdk_texture_downloader_free(download);
    g_object_unref(texture);
    gsk_render_node_unref(node);
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
    CHECK(gtk_init_check());
    fail_context = strcmp(argv[2], "context") == 0;
    bool expected_failure = fail_context || strcmp(argv[2], "unavailable") == 0;
    boxrig_gtk4_terminal_widget_config config = boxrig_gtk4_terminal_widget_config_default();
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
    CHECK(boxrig_gtk4_terminal_widget_create_with_render_state(model, &config,
        &test.widget, &state) == BOXRIG_UI_OK);
    CHECK(state.requested_policy == config.render_policy);
    CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(test.widget, &metrics) == BOXRIG_FONT_CATALOG_OK);
    feed(&test, "\033[?25l\033[38;2;255;255;255;48;2;40;80;120m GTK terminal \033[0m\r\nlocal update");
    GtkWidget *window = gtk_window_new();
    g_object_ref_sink(window);
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_widget_set_size_request(GTK_WIDGET(test.widget), 480, 160);
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(test.widget));
    CHECK(boxrig_gtk4_terminal_widget_set_visible(test.widget, true) == BOXRIG_UI_OK);
    gtk_window_present(GTK_WINDOW(window));
    GdkFrameClock *clock = gtk_widget_get_frame_clock(window);
    CHECK(clock != NULL);
    gulong painted = g_signal_connect_after(clock, "after-paint", G_CALLBACK(drawn), &test);
    await_frame(&test);
    CHECK(boxrig_gtk4_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
    CHECK(test.failed == (expected_failure && config.render_policy == BOXRIG_GTK_RENDER_OPENGL_REQUIRED));
    if (test.failed) {
        CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
        CHECK(state.reason == BOXRIG_GTK_RENDER_REASON_CONTEXT && state.status == BOXRIG_UI_UNAVAILABLE);
    } else {
        CHECK(state.active_backend == (config.render_policy == BOXRIG_GTK_RENDER_CAIRO || expected_failure
            ? BOXRIG_GTK_RENDER_BACKEND_CAIRO : BOXRIG_GTK_RENDER_BACKEND_OPENGL));
        CHECK(context_attempts == (config.render_policy == BOXRIG_GTK_RENDER_CAIRO ? 0u : 1u));
        cairo_surface_t *before = capture(GTK_WIDGET(test.widget));
        uint32_t *before_pixels = (uint32_t *)cairo_image_surface_get_data(before);
        CHECK(before_pixels[0] == 0xff285078u);
        test.generation = boxrig_gtk4_terminal_widget_presentation_generation(test.widget);
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
        cairo_surface_destroy(after);
        for (unsigned int index = 0; index < 12; index++) {
            char update[32];
            snprintf(update, sizeof(update), "\033[2;2H%c", 'A' + index);
            test.generation = boxrig_gtk4_terminal_widget_presentation_generation(test.widget);
            feed(&test, update);
            await_frame(&test);
            CHECK(!test.failed);
            after = capture(GTK_WIDGET(test.widget));
            CHECK(memcmp(cairo_image_surface_get_data(before), cairo_image_surface_get_data(after),
                (size_t)stride * metrics.cell_height) == 0);
            CHECK(memcmp(cairo_image_surface_get_data(before) + stride * metrics.cell_height,
                cairo_image_surface_get_data(after) + stride * metrics.cell_height,
                (size_t)stride * metrics.cell_height) != 0);
            cairo_surface_destroy(after);
        }
        cairo_surface_destroy(before);
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL) CHECK(target_count >= 2);
        boxrig_font_effective_metrics after_metrics;
        CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(test.widget, &after_metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(metrics.cell_width == after_metrics.cell_width && metrics.cell_height == after_metrics.cell_height);
        CHECK(boxrig_gtk4_terminal_widget_model(test.widget) == model);
        uint64_t target_generation = state.target_generation;
        test.generation = boxrig_gtk4_terminal_widget_presentation_generation(test.widget);
        test.expected_width = 640;
        test.expected_height = 240;
        gtk_window_set_default_size(GTK_WINDOW(window), test.expected_width, test.expected_height);
        await_frame(&test);
        CHECK(!test.failed);
        CHECK(boxrig_gtk4_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL)
            CHECK(state.target_generation > target_generation);
        cairo_surface_t *resized = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(resized))[0] == 0xff285078u);
        CHECK(cairo_image_surface_get_width(resized) == test.expected_width * gtk_widget_get_scale_factor(GTK_WIDGET(test.widget)));
        cairo_surface_destroy(resized);

        boxrig_font_specification font;
        CHECK(boxrig_gtk4_terminal_widget_get_font_specification(test.widget, &font) == BOXRIG_UI_OK);
        font.size_dip *= 1.5f;
        target_generation = state.target_generation;
        test.generation = boxrig_gtk4_terminal_widget_presentation_generation(test.widget);
        CHECK(boxrig_gtk4_terminal_widget_set_font_specification(test.widget, &font) == BOXRIG_UI_OK);
        await_frame(&test);
        CHECK(!test.failed);
        CHECK(boxrig_gtk4_terminal_widget_get_effective_metrics(test.widget, &after_metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(after_metrics.cell_width > metrics.cell_width && after_metrics.cell_height > metrics.cell_height);
        CHECK(boxrig_gtk4_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL)
            CHECK(state.target_generation > target_generation);
        cairo_surface_t *enlarged = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(enlarged))[0] == 0xff285078u);
        cairo_surface_destroy(enlarged);
        boxrig_ui_transparency_config transparency = boxrig_ui_transparency_config_default();
        transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
        transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        transparency.background_opacity = 64;
        test.generation = boxrig_gtk4_terminal_widget_presentation_generation(test.widget);
        CHECK(boxrig_gtk4_terminal_widget_set_transparency(test.widget, &transparency) == BOXRIG_UI_OK);
        await_frame(&test);
        CHECK(!test.failed);
        cairo_surface_t *alpha = capture(GTK_WIDGET(test.widget));
        CHECK(((uint32_t *)cairo_image_surface_get_data(alpha))[0] == 0x400a141eu);
        cairo_surface_destroy(alpha);
        if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL) {
            /* A host can complete a terminal resize after frame() queued GL
             * work and before GTK invokes render. Observe the latest model. */
            resize_before_render = test.terminal;
            test.generation = boxrig_gtk4_terminal_widget_presentation_generation(test.widget);
            feed(&test, "\033[2;2HZ");
            await_frame(&test);
            CHECK(resize_before_render == NULL && !test.failed);
            CHECK(boxrig_gtk4_terminal_widget_get_render_state(test.widget, &state) == BOXRIG_UI_OK);
            CHECK(state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL && state.status == BOXRIG_UI_OK);
        }
    }
    for (size_t index = 0; index < retained_count; index++) gsk_render_node_unref(retained_snapshots[index]);
    g_signal_handler_disconnect(clock, painted);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(test.terminal);
    printf("PASS GTK 4 widget policy=%s failure=%s backend=%d reason=%d targets=%zu compositor=%s\n",
        argv[1], argv[2], state.active_backend, state.reason, target_count,
        getenv("GSK_RENDERER") != NULL ? getenv("GSK_RENDERER") : "default");
    return 0;
}
