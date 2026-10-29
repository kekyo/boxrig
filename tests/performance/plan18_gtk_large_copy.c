/* boxrig - Under MIT. */
#include "gtk/gl_area.h"
#include "gtk/gl_surface.h"

#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "plan18 large copy failure %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

typedef struct fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    bool done;
} fixture;

static void feed(fixture *test, const char *text)
{
    size_t consumed = 0;
    size_t length = strlen(text);
    CHECK(boxrig_terminal_feed(test->terminal, (const uint8_t *)text,
        length, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    CHECK(boxrig_ui_model_synchronize(test->model) == BOXRIG_UI_OK);
}

static gboolean render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    fixture *test = data;
    (void)context;
    if (test->done) return TRUE;
    boxrig_gtk_render_state state = {0};
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    CHECK(boxrig_gtk_gl_area_describe(area, 1, 2, &config, &state) == BOXRIG_UI_OK);
    config.font_size_dip = 23;
    boxrig_gl_target destination = config.initial_target;
    CHECK(destination.width >= 3840 && destination.height >= 2160);
    fprintf(stderr, "plan18 large_copy vendor=%s renderer=%s version=%s target=%ux%u grid=193x45 font_dip=23\n",
        glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
        destination.width, destination.height);
    boxrig_gtk_gl_surface *surface = NULL;
    CHECK(boxrig_gtk_gl_surface_create(&config, &surface, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    /* Settle the first edited row before collecting one-row samples. */
    feed(test, "\033[23;2HP");
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    unsigned char primer_pixel[4];
    glBindFramebuffer(GL_READ_FRAMEBUFFER, destination.draw_framebuffer);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, primer_pixel);
    CHECK(glGetError() == GL_NO_ERROR);
    puts("case\titeration\tdraw_cpu_us\tcopy_cpu_us\trows\tcells");
    for (unsigned int kind = 0; kind < 3; kind++) {
        for (unsigned int iteration = 0; iteration < 20; iteration++) {
            if (kind == 0) {
                char sequence[32];
                int length = snprintf(sequence, sizeof(sequence), "\033[23;2H%c",
                    iteration % 2 == 0 ? 'X' : 'Y');
                CHECK(length > 0 && (size_t)length < sizeof(sequence));
                feed(test, sequence);
            } else if (kind == 1) {
                boxrig_ui_dirty dirty = {0, 45, 0, 193};
                CHECK(boxrig_ui_view_invalidate(test->view, BOXRIG_UI_DIRTY_PRESENT,
                    dirty) == BOXRIG_UI_OK);
            } else {
                /* A new retained target forces a complete redraw. Keep the
                 * dimensions and borrowed GTK framebuffer unchanged. */
                destination.generation++;
                CHECK(boxrig_gtk_gl_surface_resize(surface, &destination, &state) == BOXRIG_UI_OK);
            }
            boxrig_ogl_render_statistics before, after;
            CHECK(boxrig_gtk_gl_surface_get_statistics(surface, &before) == 0);
            gint64 started = g_get_monotonic_time();
            CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
            gint64 draw_us = g_get_monotonic_time() - started;
            started = g_get_monotonic_time();
            CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
            gint64 copy_us = g_get_monotonic_time() - started;
            CHECK(boxrig_gtk_gl_surface_get_statistics(surface, &after) == 0);
            if ((kind == 0 && (after.row_raster_count - before.row_raster_count != 1 ||
                    after.cell_raster_count - before.cell_raster_count != 193)) ||
                (kind != 0 && (after.row_raster_count - before.row_raster_count != 45 ||
                    after.cell_raster_count - before.cell_raster_count != 8685))) {
                fprintf(stderr, "plan18 large_copy unexpected raster work kind=%u iteration=%u\n",
                    kind, iteration);
                abort();
            }
            printf("%s\t%u\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\t%zu\t%zu\n",
                kind == 0 ? "one_row" : kind == 1 ? "all_rows_dirty" : "full_frame",
                iteration,
                draw_us, copy_us,
                after.row_raster_count - before.row_raster_count,
                after.cell_raster_count - before.cell_raster_count);
            /* Readback is outside the timed interval and drains prior GPU work. */
            unsigned char pixel[4];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, destination.draw_framebuffer);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
    boxrig_gtk_gl_surface_destroy(&surface);
    test->done = true;
    return TRUE;
}

static gboolean deadline(gpointer data)
{
    (void)data;
    fprintf(stderr, "plan18 large copy render timed out\n");
    abort();
}

int main(void)
{
    CHECK(gtk_init_check(NULL, NULL));
    fixture test = {0};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 45;
    terminal_config.columns = 193;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&terminal_config, &test.terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(test.terminal, &test.model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(test.model, &view_config, &test.view) == BOXRIG_UI_OK);
    feed(&test, "\033[?25l\033[48;2;20;40;60m\033[2J");
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(window);
    GtkWidget *area = boxrig_gtk_gl_area_new();
    gtk_widget_set_size_request(area, 3840, 2160);
    g_signal_connect(area, "render", G_CALLBACK(render), &test);
    gtk_container_add(GTK_CONTAINER(window), area);
    gtk_widget_show_all(window);
    guint timeout = g_timeout_add_seconds(30, deadline, NULL);
    while (!test.done) g_main_context_iteration(NULL, TRUE);
    CHECK(g_source_remove(timeout));
    gtk_widget_destroy(window);
    g_object_unref(window);
    boxrig_ui_view_destroy(test.view);
    boxrig_ui_model_destroy(test.model);
    boxrig_terminal_destroy(test.terminal);
    return 0;
}
