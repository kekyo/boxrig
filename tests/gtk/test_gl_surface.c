/* boxrig - Under MIT. */
#include "gtk/gl_area.h"
#include "gtk/gl_surface.h"

#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    abort(); } } while (0)

typedef struct fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    bool done;
    bool rgb;
} fixture;

static void feed(fixture *test, const char *text)
{
    size_t consumed;
    CHECK(boxrig_terminal_feed(test->terminal, (const uint8_t *)text,
        strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == strlen(text));
    CHECK(boxrig_ui_model_synchronize(test->model) == BOXRIG_UI_OK);
}

static uint8_t *capture(const boxrig_gtk_gl_destination *target)
{
    uint8_t *pixels = malloc((size_t)target->width * target->height * 4);
    CHECK(pixels != NULL);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target->draw_framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, (GLsizei)target->width, (GLsizei)target->height,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    CHECK(glGetError() == GL_NO_ERROR);
    return pixels;
}

static gboolean render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    fixture *test = data;
    (void)context;
    if (test->done) return TRUE;
    boxrig_gtk_render_state state = {0};
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    GLint actual_alpha = -1;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &actual_alpha);
    fprintf(stderr, "GTK %d actual alpha bits=%d rgb=%d\n", GTK_MAJOR_VERSION, actual_alpha, test->rgb);
    CHECK(actual_alpha == (test->rgb ? 0 : 8));
    boxrig_gtk_gl_destination destination;
    CHECK(boxrig_gtk_gl_area_describe(area, 1, 1, &config.runtime, &destination, &state) == BOXRIG_UI_OK);
    CHECK(destination.alpha_bits == (test->rgb ? 0u : 8u));
    const char *api = getenv("BOXRIG_TEST_GL_API");
    if (api != NULL) CHECK(state.is_gles == (strcmp(api, "gles") == 0));
    printf("GL vendor=%s renderer=%s version=%s\n", glGetString(GL_VENDOR),
        glGetString(GL_RENDERER), glGetString(GL_VERSION));
    boxrig_gtk_gl_surface *surface = NULL;
    CHECK(boxrig_gtk_gl_surface_create(&config, &destination, &surface, &state) == BOXRIG_UI_OK);
    boxrig_gl_target retained;
    CHECK(boxrig_gtk_gl_surface_get_target(surface, &retained) == BOXRIG_UI_OK);
    CHECK(retained.draw_framebuffer != destination.draw_framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, retained.draw_framebuffer);
    GLint retained_alpha = -1;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &retained_alpha);
    CHECK(retained_alpha == 8 && retained.format == BOXRIG_GL_TARGET_RGBA8);
    GLint texture = 0;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texture);
    CHECK(texture != 0);

    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_frame_requested(test->view));
    boxrig_gtk_gl_destination retained_destination = destination;
    retained_destination.draw_framebuffer = retained.draw_framebuffer;
    retained_destination.alpha_bits = 8;
    uint8_t *retained_pixels = capture(&retained_destination);
    for (size_t pixel = 0; pixel < (size_t)destination.width * destination.height; pixel++)
        CHECK(retained_pixels[pixel * 4 + 3] == 255);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(test->view));
    uint8_t *before = capture(&destination);
    CHECK(memcmp(before, retained_pixels, (size_t)destination.width * destination.height * 4) == 0);
    free(retained_pixels);
    for (size_t pixel = 0; pixel < (size_t)destination.width * destination.height; pixel++)
        CHECK(before[pixel * 4 + 3] == 255);
    boxrig_ogl_render_statistics baseline;
    CHECK(boxrig_gtk_gl_surface_get_statistics(surface, &baseline) == 0);
    CHECK(baseline.draw_batch_count > 0 && baseline.grayscale_glyph_upload_count > 0);

    feed(test, "\033[2;2HX");
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    /* Simulate GTK rotating to storage with no previous terminal pixels. A
     * poisoned destination must receive the whole retained image, including
     * unchanged rows. The scissor state must not clip that transfer. */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination.draw_framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(1, 0, 1, 0.25f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    CHECK(glIsEnabled(GL_SCISSOR_TEST));
    glDisable(GL_SCISSOR_TEST);
    uint8_t *after = capture(&destination);
    unsigned int cell_height = boxrig_gtk_gl_surface_cell_height(surface);
    size_t changed = 0;
    for (unsigned int y = 0; y < destination.height; y++) {
        unsigned int from_top = destination.height - y - 1;
        for (unsigned int x = 0; x < destination.width; x++) {
            size_t at = ((size_t)y * destination.width + x) * 4;
            if (from_top < cell_height || from_top >= cell_height * 2) {
                CHECK(memcmp(before + at, after + at, 4) == 0);
            } else if (memcmp(before + at, after + at, 4) != 0) changed++;
        }
    }
    CHECK(changed > 0);
    boxrig_ogl_render_statistics incremental;
    CHECK(boxrig_gtk_gl_surface_get_statistics(surface, &incremental) == 0);
    CHECK(incremental.row_raster_count - baseline.row_raster_count == 1);
    CHECK(incremental.full_frame_software_upload_count == 0 && incremental.readback_count == 0 &&
        incremental.swap_count == 0 && incremental.finish_count == 0 && incremental.wait_count == 0);

    feed(test, "\033[3;1Hpending");
    boxrig_gtk_gl_surface_fail_next(surface, BOXRIG_OGL_FAILURE_DRAW);
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) != BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_frame_requested(test->view));
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) != BOXRIG_UI_OK);
    uint8_t *failed = capture(&destination);
    CHECK(memcmp(after, failed, (size_t)destination.width * destination.height * 4) == 0);
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(test->view));

    /* A stale destination must not consume a newly prepared frame. */
    feed(test, "\033[4;1Hstale target");
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    boxrig_gtk_gl_destination invalid = destination;
    invalid.generation = 0;
    CHECK(boxrig_gtk_gl_surface_present(surface, &invalid, &state) != BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_frame_requested(test->view));
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);

    /* An incomplete destination must not accept successfully prepared dirty. */
    feed(test, "\033[4;1Hretry");
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    GLuint incomplete = 0;
    glGenFramebuffers(1, &incomplete);
    invalid = destination;
    invalid.draw_framebuffer = incomplete;
    CHECK(boxrig_gtk_gl_surface_present(surface, &invalid, &state) != BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_frame_requested(test->view));
    glDeleteFramebuffers(1, &incomplete);
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(test->view));

    boxrig_ui_transparency_capabilities capabilities = {0};
    capabilities.premultiplied_argb32_target = true;
    capabilities.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    CHECK(boxrig_ui_view_report_transparency_capabilities(test->view, &capabilities) == BOXRIG_UI_OK);
    boxrig_ui_transparency_config transparency = boxrig_ui_transparency_config_default();
    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.background_opacity = 64;
    CHECK(boxrig_ui_view_set_transparency_config(test->view, &transparency) == BOXRIG_UI_OK);
#if GTK_MAJOR_VERSION == 3
    if (test->rgb) {
        /* Recreate real GTK buffers before restoring alpha presentation. */
        gtk_gl_area_set_has_alpha(area, TRUE);
        gtk_gl_area_attach_buffers(area);
        CHECK(boxrig_gtk_gl_area_describe(area, 1, 2, &config.runtime,
            &destination, &state) == BOXRIG_UI_OK);
        CHECK(destination.alpha_bits == 8 && destination.generation == 2);
    }
#endif
    CHECK(boxrig_gtk_gl_surface_draw(surface, test->view, NULL, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_gtk_gl_surface_present(surface, &destination, &state) == BOXRIG_UI_OK);
    uint8_t *transparent = capture(&destination);
    CHECK(transparent[3] == 64);
    CHECK(transparent[0] == 4 && transparent[1] == 4 && transparent[2] == 4);
    free(transparent);

    transparency = boxrig_ui_transparency_config_default();
    CHECK(boxrig_ui_view_set_transparency_config(test->view, &transparency) == BOXRIG_UI_OK);
    const GLint formats[] = {GL_RGBA8, GL_RGB8, GL_SRGB8_ALPHA8, GL_SRGB8};
    for (size_t format = 0; format < G_N_ELEMENTS(formats); format++) {
        bool rgb = format % 2 != 0;
        GLuint buffer, output_texture;
        glGenFramebuffers(1, &buffer);
        glGenTextures(1, &output_texture);
        glBindTexture(GL_TEXTURE_2D, output_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, formats[format], (GLsizei)destination.width,
            (GLsizei)destination.height, 0, rgb ? GL_RGB : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, buffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture, 0);
        bool supported = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        CHECK(glGetError() == GL_NO_ERROR);
        /* GLES 3.0 does not require RGB sRGB textures to be color renderable. */
        if (!supported) {
            CHECK(state.is_gles && formats[format] == GL_SRGB8);
            printf("UNAVAILABLE GLES SRGB8 framebuffer; linear RGB8 and SRGB8_ALPHA8 remain required\n");
        } else {
            boxrig_gtk_gl_destination output = destination;
            output.draw_framebuffer = buffer;
            output.srgb = format >= 2;
            output.alpha_bits = rgb ? 0 : 8;
            output.generation = format + 3;
            boxrig_ogl_renderer_config transfer_config = config;
            transfer_config.runtime.target_generation = output.generation;
            boxrig_gtk_gl_surface *transfer = NULL;
            CHECK(boxrig_gtk_gl_surface_create(&transfer_config, &output, &transfer, &state) == BOXRIG_UI_OK);
            CHECK(boxrig_gtk_gl_surface_draw(transfer, test->view, NULL, &state) == BOXRIG_UI_OK);
            CHECK(boxrig_gtk_gl_surface_present(transfer, &output, &state) == BOXRIG_UI_OK);
            uint8_t *pixels = capture(&output);
            size_t at = (size_t)(output.height - 1) * output.width * 4;
            const int expected[] = {40, 80, 120, 255};
            for (size_t channel = 0; channel < 3; channel++)
                CHECK(abs((int)pixels[at + channel] - expected[channel]) <= 3);
            CHECK(pixels[at + 3] == 255);
            free(pixels);
            boxrig_gtk_gl_surface_destroy(&transfer);
            printf("PASS transfer format=0x%x alpha=%u srgb=%d\n", formats[format], output.alpha_bits, output.srgb);
        }
        glDeleteFramebuffers(1, &buffer);
        glDeleteTextures(1, &output_texture);
    }

    boxrig_gtk_gl_surface_destroy(&surface);
    CHECK(surface == NULL);
    CHECK(!glIsFramebuffer(retained.draw_framebuffer));
    CHECK(!glIsTexture((GLuint)texture));
    CHECK(glIsFramebuffer(destination.draw_framebuffer));
    free(before);
    free(after);
    free(failed);
    glBindFramebuffer(GL_FRAMEBUFFER, destination.draw_framebuffer);
    test->done = true;
    printf("PASS GTK %d retained GL pixels, incremental work, deferred acceptance, failure retry and resource release\n", GTK_MAJOR_VERSION);
    return TRUE;
}

static gboolean deadline(gpointer data)
{
    (void)data;
    fprintf(stderr, "FAIL retained GL test did not render\n");
    abort();
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || (argc == 2 && strcmp(argv[1], "rgb") == 0));
#if GTK_MAJOR_VERSION == 3
    CHECK(gtk_init_check(NULL, NULL));
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
    CHECK(gtk_init_check());
    GtkWidget *window = gtk_window_new();
#endif
    g_object_ref_sink(window);
    fixture test = {0};
    test.rgb = argc == 2;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 4;
    terminal_config.columns = 40;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&terminal_config, &test.terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(test.terminal, &test.model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(test.model, &view_config, &test.view) == BOXRIG_UI_OK);
    feed(&test, "\033[?25l\033[38;2;255;255;255;48;2;40;80;120m unchanged row \033[0m\r\nlocal update");
    GtkWidget *area = boxrig_gtk_gl_area_new();
#if GTK_MAJOR_VERSION == 3
    if (test.rgb) gtk_gl_area_set_has_alpha(GTK_GL_AREA(area), FALSE);
#else
    CHECK(!test.rgb);
#endif
    gtk_widget_set_size_request(area, 480, 160);
    g_signal_connect(area, "render", G_CALLBACK(render), &test);
#if GTK_MAJOR_VERSION == 3
    gtk_container_add(GTK_CONTAINER(window), area);
    gtk_widget_show_all(window);
#else
    gtk_window_set_child(GTK_WINDOW(window), area);
    gtk_window_present(GTK_WINDOW(window));
#endif
    guint timeout = g_timeout_add_seconds(30, deadline, NULL);
    while (!test.done) g_main_context_iteration(NULL, TRUE);
    CHECK(g_source_remove(timeout));
#if GTK_MAJOR_VERSION == 3
    gtk_widget_destroy(window);
#else
    gtk_window_destroy(GTK_WINDOW(window));
#endif
    g_object_unref(window);
    boxrig_ui_view_destroy(test.view);
    boxrig_ui_model_destroy(test.model);
    boxrig_terminal_destroy(test.terminal);
    return 0;
}
