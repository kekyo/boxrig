/* boxrig - Under MIT. */
#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION == 3
#include "boxrig/gtk3.h"
typedef BoxrigGtk3TerminalWidget terminal_widget;
typedef boxrig_gtk3_terminal_widget_config widget_config;
#define API(name) boxrig_gtk3_terminal_widget_##name
#else
#include "boxrig/gtk4.h"
typedef BoxrigGtk4TerminalWidget terminal_widget;
typedef boxrig_gtk4_terminal_widget_config widget_config;
#define API(name) boxrig_gtk4_terminal_widget_##name
#endif
#include "gtk/gl_surface.h"
#include "pangocairo/renderer.h"
#include <epoxy/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL GTK %d %s:%d: %s\n", GTK_MAJOR_VERSION, __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

static unsigned int context_attempts;
static size_t live_surfaces;
static size_t created_surfaces;
static boxrig_gtk_gl_surface *surfaces[64];
static GtkWidget *areas[64];
static size_t area_count;
static bool fail_context;
static bool fail_resource;
static bool fail_cairo;
static bool lose_current_context;
static bool failed_dirty_retained;
static boxrig_ui_view *observed_view;
static terminal_widget *observed_widget;
static bool probe_reentry;

/** @copydoc gtk_gl_area_new */
GtkWidget *__real_gtk_gl_area_new(void);
/** @copydoc boxrig_gtk_gl_surface_create */
boxrig_ui_status __real_boxrig_gtk_gl_surface_create(const boxrig_ogl_renderer_config *config,
    const boxrig_gtk_gl_destination *destination, boxrig_gtk_gl_surface **surface_out, boxrig_gtk_render_state *state);
/** @copydoc boxrig_gtk_gl_surface_destroy */
void __real_boxrig_gtk_gl_surface_destroy(boxrig_gtk_gl_surface **surface);
/** @copydoc boxrig_gtk_gl_surface_draw */
boxrig_ui_status __real_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition, boxrig_gtk_render_state *state);
/** @copydoc boxrig_gtk_gl_surface_present */
boxrig_ui_status __real_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gtk_gl_destination *destination, boxrig_gtk_render_state *state);
/** @copydoc boxrig_pango_cairo_renderer_draw_view_composition */
int __real_boxrig_pango_cairo_renderer_draw_view_composition(boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view, boxrig_composition *composition);

static GdkGLContext *creating_context(GtkGLArea *area, gpointer data)
{
    (void)data;
    context_attempts++;
    if (fail_context) {
        GError *error = g_error_new_literal(GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE, "Injected context failure");
        gtk_gl_area_set_error(area, error);
        g_error_free(error);
        g_signal_stop_emission_by_name(area, "create-context");
    }
    return NULL;
}

/** Counts actual context requests without changing the product policy. */
GtkWidget *__wrap_gtk_gl_area_new(void)
{
    GtkWidget *area = __real_gtk_gl_area_new();
    CHECK(area_count < G_N_ELEMENTS(areas));
    areas[area_count] = area;
    g_object_add_weak_pointer(G_OBJECT(area), (gpointer *)&areas[area_count++]);
    g_signal_connect(area, "create-context", G_CALLBACK(creating_context), NULL);
    return area;
}

/** Tracks successfully created retained surfaces across switches. */
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_create(const boxrig_ogl_renderer_config *config,
    const boxrig_gtk_gl_destination *destination, boxrig_gtk_gl_surface **surface_out, boxrig_gtk_render_state *state)
{
    if (fail_resource) {
        fail_resource = false;
        *surface_out = NULL;
        state->status = BOXRIG_UI_OUT_OF_MEMORY;
        state->reason = BOXRIG_GTK_RENDER_REASON_RESOURCE;
        snprintf(state->message, sizeof(state->message), "Injected retained resource failure");
        return state->status;
    }
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_create(config, destination, surface_out, state);
    if (status == BOXRIG_UI_OK) {
        CHECK(created_surfaces < G_N_ELEMENTS(surfaces));
        surfaces[created_surfaces++] = *surface_out;
        live_surfaces++;
    }
    return status;
}

/** Ensures replaced surfaces are released by the widget. */
void __wrap_boxrig_gtk_gl_surface_destroy(boxrig_gtk_gl_surface **surface)
{
    if (surface != NULL && *surface != NULL) {
        for (size_t index = 0; index < created_surfaces; index++) {
            if (surfaces[index] == *surface) {
                boxrig_ogl_render_statistics statistics;
                CHECK(boxrig_gtk_gl_surface_get_statistics(*surface, &statistics) == 0);
                CHECK(statistics.full_frame_software_upload_count == 0 && statistics.readback_count == 0 &&
                    statistics.swap_count == 0 && statistics.finish_count == 0 && statistics.wait_count == 0);
                surfaces[index] = NULL;
                CHECK(live_surfaces > 0);
                live_surfaces--;
                break;
            }
        }
    }
    __real_boxrig_gtk_gl_surface_destroy(surface);
}

/** Asserts failed preparation leaves the view's update available. */
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition, boxrig_gtk_render_state *state)
{
    if (probe_reentry) {
        probe_reentry = false;
        CHECK(API(set_render_policy)(observed_widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_BUSY);
        boxrig_ui_component_frame_result result;
        CHECK(API(frame)(observed_widget, &result) == BOXRIG_UI_BUSY);
    }
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_draw(surface, view, composition, state);
    if (status != BOXRIG_UI_OK) {
        CHECK(boxrig_ui_view_frame_requested(view));
        failed_dirty_retained = true;
    }
    return status;
}

/** Simulates loss of the current context at the presentation boundary. */
boxrig_ui_status __wrap_boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gtk_gl_destination *destination, boxrig_gtk_render_state *state)
{
    GdkGLContext *context = NULL;
    if (lose_current_context) {
        lose_current_context = false;
        context = gdk_gl_context_get_current();
        CHECK(context != NULL);
        gdk_gl_context_clear_current();
    }
    boxrig_ui_status status = __real_boxrig_gtk_gl_surface_present(surface, destination, state);
    if (status == BOXRIG_UI_OK && destination->alpha_bits == 0) {
        /* Selection, preedit and the scrollbar coexist in this fixture. The
         * RGB optimization must keep the entire retained image opaque. */
        boxrig_gl_target retained;
        CHECK(boxrig_gtk_gl_surface_get_target(surface, &retained) == BOXRIG_UI_OK);
        GLint previous;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, retained.draw_framebuffer);
        size_t count = (size_t)retained.width * retained.height;
        uint8_t *pixels = malloc(count * 4);
        CHECK(pixels != NULL);
        glReadPixels(0, 0, (GLsizei)retained.width, (GLsizei)retained.height,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        CHECK(glGetError() == GL_NO_ERROR);
        for (size_t pixel = 0; pixel < count; pixel++) CHECK(pixels[pixel * 4 + 3] == 255);
        free(pixels);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous);
    }
    if (context != NULL) gdk_gl_context_make_current(context);
    if (status != BOXRIG_UI_OK) {
        CHECK(boxrig_ui_view_frame_requested(observed_view));
        failed_dirty_retained = true;
    }
    return status;
}

/** Rejects one Cairo candidate without modifying the last complete GL image. */
int __wrap_boxrig_pango_cairo_renderer_draw_view_composition(boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view, boxrig_composition *composition)
{
    if (fail_cairo) {
        fail_cairo = false;
        return -1;
    }
    return __real_boxrig_pango_cairo_renderer_draw_view_composition(renderer, view, composition);
}

typedef struct fixture {
    terminal_widget *widget;
    boxrig_terminal *terminal;
    boxrig_gtk_render_backend expected;
    uint64_t previous_generation;
    uint64_t painted_generation;
    bool done;
    boxrig_ui_status expected_frame_status;
} fixture;

static void painted(GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    fixture *test = data;
    test->painted_generation = API(presentation_generation)(test->widget);
}

static gboolean frame(gpointer data)
{
    fixture *test = data;
    boxrig_ui_layout_result layout;
    if (API(get_layout)(test->widget, &layout) != BOXRIG_UI_OK) return G_SOURCE_CONTINUE;
    boxrig_ui_component_frame_result result;
    boxrig_ui_status status = API(frame)(test->widget, &result);
    if (status != BOXRIG_UI_OK) {
        CHECK(status == test->expected_frame_status);
        boxrig_gtk_render_state state;
        CHECK(API(get_render_state)(test->widget, &state) == BOXRIG_UI_OK);
        CHECK(!state.pending && state.active_backend == test->expected);
        CHECK(boxrig_ui_view_frame_requested(API(view)(test->widget)));
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    boxrig_ui_layout_action action;
    while (API(take_layout_action)(test->widget, &action)) {
        CHECK(action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(test->terminal, action.value.grid_resize.rows,
            action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(API(complete_grid_resize)(test->widget, action.revision, true) == BOXRIG_UI_OK);
    }
    boxrig_gtk_render_state state;
    CHECK(API(get_render_state)(test->widget, &state) == BOXRIG_UI_OK);
    if (!state.pending && state.active_backend == test->expected &&
        API(presentation_generation)(test->widget) > test->previous_generation &&
        test->painted_generation == API(presentation_generation)(test->widget) &&
        !result.frame_pending && !boxrig_ui_view_frame_requested(API(view)(test->widget))) {
        test->done = true;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean expired(gpointer data)
{
    fixture *test = data;
    boxrig_gtk_render_state state;
    CHECK(API(get_render_state)(test->widget, &state) == BOXRIG_UI_OK);
    fprintf(stderr, "switch timeout pending=%d active=%d requested=%d status=%d reason=%d message=%s\n",
        state.pending, state.active_backend, state.requested_policy, state.status, state.reason, state.message);
    abort();
}

static void await_frame(fixture *test, boxrig_gtk_render_backend expected)
{
    test->expected = expected;
    test->done = false;
    g_idle_add(frame, test);
    guint timeout = g_timeout_add_seconds(30, expired, test);
    while (!test->done) g_main_context_iteration(NULL, true);
    CHECK(g_source_remove(timeout));
    test->previous_generation = API(presentation_generation)(test->widget);
}

static void invalidate(fixture *test)
{
    boxrig_ui_dirty dirty = {.row = 0, .column = 0, .row_count = 1, .column_count = 1};
    CHECK(boxrig_ui_view_invalidate(API(view)(test->widget), BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
}

static uint32_t first_pixel(GtkWidget *widget)
{
#if GTK_MAJOR_VERSION == 3
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(image);
    gtk_widget_draw(widget, cr);
    cairo_destroy(cr);
#else
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gtk_widget_snapshot_child(gtk_widget_get_parent(widget), widget, snapshot);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(
        gtk_native_get_renderer(gtk_widget_get_native(widget)), node, &bounds);
    CHECK(texture != NULL);
    cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    GdkTextureDownloader *download = gdk_texture_downloader_new(texture);
    gdk_texture_downloader_set_format(download, GDK_MEMORY_DEFAULT);
    gdk_texture_downloader_download_into(download, cairo_image_surface_get_data(image), cairo_image_surface_get_stride(image));
    gdk_texture_downloader_free(download);
    g_object_unref(texture);
    gsk_render_node_unref(node);
#endif
    cairo_surface_flush(image);
    uint32_t pixel = ((uint32_t *)cairo_image_surface_get_data(image))[0];
    cairo_surface_destroy(image);
    return pixel;
}

int main(void)
{
#if GTK_MAJOR_VERSION == 3
    CHECK(gtk_init_check(NULL, NULL));
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    CHECK(gtk_init_check());
    GtkWidget *window = gtk_window_new();
#endif
    g_object_ref_sink(window);
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    fixture test = {0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 6;
    config.columns = 50;
    CHECK(boxrig_terminal_create(&config, &test.terminal) == BOXRIG_STATUS_OK);
    const char scene[] = "\033[?25l\033[48;2;40;80;120m\033[2J";
    size_t consumed;
    CHECK(boxrig_terminal_feed(test.terminal, (const uint8_t *)scene, sizeof(scene) - 1, &consumed) == BOXRIG_STATUS_OK);
    for (size_t row = 0; row < 40; row++) {
        const char line[] = "runtime selection and history\r\n";
        CHECK(boxrig_terminal_feed(test.terminal, (const uint8_t *)line, sizeof(line) - 1, &consumed) == BOXRIG_STATUS_OK);
    }
    boxrig_ui_model *model = NULL;
    CHECK(boxrig_ui_model_create(test.terminal, &model) == BOXRIG_UI_OK);
    widget_config wc = API(config_default)();
    boxrig_gtk_render_state state;
    wc.view.maximum_selection_rows = 0;
    CHECK(API(create_with_render_state)(model, &wc, &test.widget, &state) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(test.widget == NULL && !state.pending && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
    wc = API(config_default)();
    CHECK(API(create_with_render_state)(model, &wc, &test.widget, &state) == BOXRIG_UI_OK);
    CHECK(state.pending && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
    gtk_widget_set_size_request(GTK_WIDGET(test.widget), 480, 160);
#if GTK_MAJOR_VERSION == 3
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(test.widget));
    gtk_widget_show_all(window);
#else
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(test.widget));
    gtk_window_present(GTK_WINDOW(window));
#endif
    CHECK(API(set_visible)(test.widget, true) == BOXRIG_UI_OK);
    GdkFrameClock *clock = gtk_widget_get_frame_clock(window);
    CHECK(clock != NULL);
    gulong painted_handler = g_signal_connect_after(clock, "after-paint", G_CALLBACK(painted), &test);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    CHECK(context_attempts == 0 && live_surfaces == 0);
    CHECK(API(set_render_policy)(test.widget, (boxrig_gtk_render_policy)99) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(API(set_render_policy)(NULL, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_ui_view *view = API(view)(test.widget);
    observed_view = view;
    observed_widget = test.widget;
    boxrig_composition *composition = API(composition)(test.widget);
    boxrig_font_effective_metrics metrics;
    CHECK(API(get_effective_metrics)(test.widget, &metrics) == BOXRIG_FONT_CATALOG_OK);
    bool changed;
    CHECK(API(apply_scrollbar_action)(test.widget, BOXRIG_SCROLLBAR_POSITION, 0, &changed) == BOXRIG_UI_OK);
    CHECK(changed);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    boxrig_scrollbar_model history;
    CHECK(API(get_scrollbar_model)(test.widget, &history) == BOXRIG_UI_OK);
    CHECK(history.viewport_offset > 0);
    boxrig_ui_hit_test_result hit;
    CHECK(boxrig_ui_view_hit_test(view, metrics.cell_width * BOXRIG_UI_FIXED_ONE,
        metrics.cell_height * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, metrics.cell_width * 5 * BOXRIG_UI_FIXED_ONE,
        metrics.cell_height * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    boxrig_text_snapshot *selection = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &selection) == BOXRIG_UI_OK);
    size_t selected_length;
    const uint8_t *selected = boxrig_text_snapshot_data(selection, &selected_length);
    CHECK(selected_length > 0);
    CHECK(boxrig_composition_focus_changed(composition, true) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event preedit = {0};
    preedit.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    preedit.text = "preedit";
    preedit.text_length = 7;
    preedit.caret_offset = 3;
    preedit.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    size_t rows, columns;
    boxrig_terminal_size(test.terminal, &rows, &columns);
    uint32_t pixel = first_pixel(GTK_WIDGET(test.widget));
    for (size_t index = 0; index < 12; index++) {
        bool gl = index % 2 == 0;
        boxrig_gtk_render_backend previous = gl ? BOXRIG_GTK_RENDER_BACKEND_CAIRO : BOXRIG_GTK_RENDER_BACKEND_OPENGL;
        boxrig_gtk_render_backend next = gl ? BOXRIG_GTK_RENDER_BACKEND_OPENGL : BOXRIG_GTK_RENDER_BACKEND_CAIRO;
        CHECK(API(set_render_policy)(test.widget, gl ? BOXRIG_GTK_RENDER_OPENGL_REQUIRED : BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.pending && state.active_backend == previous);
        await_frame(&test, next);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(!state.pending && state.status == BOXRIG_UI_OK && state.active_backend == next);
        CHECK(first_pixel(GTK_WIDGET(test.widget)) == pixel);
        CHECK(API(model)(test.widget) == model && API(view)(test.widget) == view && API(composition)(test.widget) == composition);
        boxrig_font_effective_metrics current;
        CHECK(API(get_effective_metrics)(test.widget, &current) == BOXRIG_FONT_CATALOG_OK);
        CHECK(current.cell_width == metrics.cell_width && current.cell_height == metrics.cell_height && current.layout_revision == metrics.layout_revision);
        boxrig_scrollbar_model current_history;
        CHECK(API(get_scrollbar_model)(test.widget, &current_history) == BOXRIG_UI_OK);
        CHECK(current_history.viewport_offset == history.viewport_offset);
        boxrig_text_snapshot *current_selection = NULL;
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &current_selection) == BOXRIG_UI_OK);
        size_t length;
        const uint8_t *bytes = boxrig_text_snapshot_data(current_selection, &length);
        CHECK(length == selected_length && memcmp(bytes, selected, length) == 0);
        boxrig_text_snapshot_destroy(current_selection);
        boxrig_composition_view composed;
        CHECK(boxrig_composition_get_view(composition, &composed) == BOXRIG_COMPOSITION_OK);
        CHECK(composed.text_length == 7 && memcmp(composed.text, "preedit", 7) == 0 && composed.caret_offset == 3);
        size_t current_rows, current_columns;
        boxrig_terminal_size(test.terminal, &current_rows, &current_columns);
        CHECK(current_rows == rows && current_columns == columns);
        CHECK(live_surfaces == (gl ? 1u : 0u));
    }
    CHECK(context_attempts == 6);
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    unsigned int before_cancel = context_attempts;
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
    CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
    CHECK(!state.pending && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    CHECK(context_attempts == before_cancel && live_surfaces == 1);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    fail_cairo = true;
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    CHECK(!fail_cairo && live_surfaces == 1);
    CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
    CHECK(state.requested_policy == BOXRIG_GTK_RENDER_CAIRO && state.status != BOXRIG_UI_OK);
    CHECK(first_pixel(GTK_WIDGET(test.widget)) == pixel);
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    for (size_t fault = 0; fault < 2; fault++) {
        fail_context = fault == 0;
        fail_resource = fault == 1;
        CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.status != BOXRIG_UI_OK && state.reason == (fault == 0
            ? BOXRIG_GTK_RENDER_REASON_CONTEXT : BOXRIG_GTK_RENDER_REASON_RESOURCE));
        CHECK(first_pixel(GTK_WIDGET(test.widget)) == pixel && live_surfaces == 0);
        unsigned int attempts = context_attempts;
        invalidate(&test);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
        CHECK(context_attempts == attempts);
        fail_context = false;
        CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
        CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    }
    for (size_t fault = 0; fault < 3; fault++) {
        CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_PREFERRED) == BOXRIG_UI_OK);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
        failed_dirty_retained = false;
        if (fault == 2) lose_current_context = true;
        else boxrig_gtk_gl_surface_fail_next(surfaces[created_surfaces - 1],
            fault == 0 ? BOXRIG_OGL_FAILURE_DRAW : BOXRIG_OGL_FAILURE_FENCE);
        invalidate(&test);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
        CHECK(failed_dirty_retained && live_surfaces == 0);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.status != BOXRIG_UI_OK && state.reason == (fault == 2
            ? BOXRIG_GTK_RENDER_REASON_CONTEXT_LOST : BOXRIG_GTK_RENDER_REASON_DRAW));
        CHECK(first_pixel(GTK_WIDGET(test.widget)) == pixel);
    }
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    boxrig_gtk_gl_surface_fail_next(surfaces[created_surfaces - 1], BOXRIG_OGL_FAILURE_DRAW);
    test.expected_frame_status = BOXRIG_UI_UNAVAILABLE;
    invalidate(&test);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_NONE);
    test.expected_frame_status = BOXRIG_UI_OK;
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_CAIRO) == BOXRIG_UI_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_CAIRO);
    CHECK(API(set_render_policy)(test.widget, BOXRIG_GTK_RENDER_OPENGL_REQUIRED) == BOXRIG_UI_OK);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    probe_reentry = true;
    invalidate(&test);
    await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
    CHECK(!probe_reentry);
    boxrig_text_snapshot *after_failures = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &after_failures) == BOXRIG_UI_OK);
    size_t after_length;
    const uint8_t *after_text = boxrig_text_snapshot_data(after_failures, &after_length);
    CHECK(after_length == selected_length && memcmp(after_text, selected, after_length) == 0);
    boxrig_text_snapshot_destroy(after_failures);
    boxrig_composition_view after_preedit;
    CHECK(boxrig_composition_get_view(composition, &after_preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(after_preedit.text_length == 7 && memcmp(after_preedit.text, "preedit", 7) == 0);
    for (size_t index = 0; index < 3; index++) {
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        uint64_t context_generation = state.context_generation;
        uint64_t target_generation = state.target_generation;
        g_signal_handler_disconnect(clock, painted_handler);
        gtk_widget_set_visible(window, false);
        gtk_widget_unrealize(window);
        CHECK(live_surfaces == 0);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.pending && state.active_backend == BOXRIG_GTK_RENDER_BACKEND_NONE);
#if GTK_MAJOR_VERSION == 3
        gtk_widget_show_all(window);
#else
        gtk_window_present(GTK_WINDOW(window));
#endif
        clock = gtk_widget_get_frame_clock(window);
        CHECK(clock != NULL);
        painted_handler = g_signal_connect_after(clock, "after-paint", G_CALLBACK(painted), &test);
        await_frame(&test, BOXRIG_GTK_RENDER_BACKEND_OPENGL);
        CHECK(API(get_render_state)(test.widget, &state) == BOXRIG_UI_OK);
        CHECK(state.context_generation > context_generation && state.target_generation > target_generation);
        CHECK(live_surfaces == 1 && API(model)(test.widget) == model && API(view)(test.widget) == view);
        CHECK(first_pixel(GTK_WIDGET(test.widget)) == pixel);
    }
    boxrig_text_snapshot_destroy(selection);
    g_signal_handler_disconnect(clock, painted_handler);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(window);
    CHECK(live_surfaces == 0);
    for (size_t index = 0; index < area_count; index++) CHECK(areas[index] == NULL);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(test.terminal);
    printf("PASS GTK %d runtime policy switches preserve terminal, view, metrics, history, selection and preedit; contexts=%u surfaces=%zu\n",
        GTK_MAJOR_VERSION, context_attempts, created_surfaces);
    return 0;
}
