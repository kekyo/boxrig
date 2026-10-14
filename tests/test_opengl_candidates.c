/* boxrig - Under MIT. */
#define GL_GLEXT_PROTOTYPES 1
#include "boxrig/render/opengl.h"
#include "opengl_host.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL OpenGL candidates %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)
#define WIDTH 480U
#define HEIGHT 320U

static unsigned char *capture(uint32_t framebuffer)
{
    unsigned char *pixels = malloc(WIDTH * HEIGHT * 4);
    CHECK(pixels != NULL);
    GLint previous;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    CHECK(glGetError() == GL_NO_ERROR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous);
    return pixels;
}

static size_t differences(const unsigned char *a, const unsigned char *b)
{
    size_t count = 0;
    for (size_t i = 0; i < WIDTH * HEIGHT; i++)
        if (memcmp(a + i * 4, b + i * 4, 4) != 0) count++;
    return count;
}

int main(void)
{
    opengl_host *host;
    opengl_host_report report;
    CHECK(opengl_host_create("egl", &host, &report));
    uint32_t texture;
    uint32_t framebuffer = opengl_host_create_framebuffer(host,
        BOXRIG_GL_TARGET_RGBA8, WIDTH, HEIGHT, &texture);
    CHECK(framebuffer != 0);
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    config.runtime.resolve = opengl_host_resolve;
    config.runtime.resolver_context = host;
    config.runtime.query_current_context = opengl_host_query_current;
    config.runtime.current_context_query_context = host;
    config.runtime.context_generation = config.runtime.target_generation = 1;
    config.initial_target = (boxrig_gl_target){.draw_framebuffer = framebuffer,
        .width = WIDTH, .height = HEIGHT, .generation = 1,
        .format = BOXRIG_GL_TARGET_RGBA8, .alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED,
        .origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT};
    boxrig_ogl_renderer *renderer;
    CHECK(boxrig_ogl_renderer_create(&config, &renderer, NULL) == BOXRIG_GL_OK);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 20; tc.columns = 60;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    vc.blink.cursor_enabled = vc.blink.text_enabled = false;
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25l", 6, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *before = capture(framebuffer);
    const char *items[] = {"記者", "汽車🙂", "帰社", "貴社", "喜捨"};
    const char *labels[] = {"a", "s", "d"};
    boxrig_ui_candidate_snapshot snapshot = {.items = items, .labels = labels,
        .count = 5, .page_size = 3, .cursor = 1, .visible = true, .cursor_visible = true,
        .auxiliary_visible = true, .auxiliary = "説明\n次の行"};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *shown = capture(framebuffer);
    CHECK(differences(before, shown) > 128);
    boxrig_ui_candidate_view actual;
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.layout.count == 3 && actual.layout.revision == actual.revision);
    boxrig_ogl_render_statistics initial, repeated;
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer, &initial) == 0);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *unchanged = capture(framebuffer);
    CHECK(differences(shown, unchanged) == 0);
    CHECK(boxrig_ogl_renderer_get_render_statistics(renderer, &repeated) == 0);
    CHECK(repeated.upload_bytes == initial.upload_bytes);
    free(unchanged);
    snapshot.visible = snapshot.auxiliary_visible = false;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *hidden = capture(framebuffer);
    CHECK(differences(before, hidden) == 0);
    free(hidden);
    snapshot.visible = snapshot.auxiliary_visible = true;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *reshown = capture(framebuffer);
    CHECK(differences(shown, reshown) == 0);
    free(reshown);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[20;60H", 8, &consumed) == BOXRIG_STATUS_OK);
    snapshot.cursor = 4;
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *moved = capture(framebuffer);
    CHECK(differences(shown, moved) > 128);
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.layout.first == 3 && actual.layout.count == 2);
    CHECK(actual.layout.bounds.x >= 0 && actual.layout.bounds.y >= 0);
    CHECK(actual.layout.bounds.x + actual.layout.bounds.width <= (int32_t)WIDTH);
    CHECK(actual.layout.bounds.y + actual.layout.bounds.height <= (int32_t)HEIGHT);
    free(moved);
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *cleared = capture(framebuffer);
    CHECK(differences(before, cleared) == 0);
    free(cleared);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    snapshot = (boxrig_ui_candidate_snapshot){.format = BOXRIG_UI_CANDIDATE_PAGE,
        .auxiliary_above = "候補なしの補助情報", .auxiliary_above_visible = true};
    CHECK(boxrig_ui_view_set_candidates(view, &snapshot) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    unsigned char *panel = capture(framebuffer);
    CHECK(differences(before, panel) > 128);
    CHECK(boxrig_ui_view_get_candidates(view, &actual) == BOXRIG_UI_OK);
    CHECK(actual.layout.count == 0 && actual.layout.auxiliary_above.height > 0);
    CHECK(actual.layout.bounds.x > 0 && actual.layout.bounds.y > 0);
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
    cleared = capture(framebuffer);
    CHECK(differences(before, cleared) == 0);
    free(cleared); free(panel); free(shown); free(before);
    CHECK(boxrig_ogl_renderer_destroy(&renderer, 1, NULL) == BOXRIG_GL_OK);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    opengl_host_delete_framebuffer(host, framebuffer, texture);
    opengl_host_destroy(host);
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS OpenGL candidate display, idle reuse, hide, page placement and focus dirty");
    return 0;
}
