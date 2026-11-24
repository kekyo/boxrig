/* Measure pause through first draw on a fresh PangoCairo image surface. */
#define _POSIX_C_SOURCE 200809L
#include "../../src/pangocairo/renderer.h"
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { rows = 24, columns = 80, history_limit = 10000, wire_limit = 64 * 1024 * 1024 };

static double milliseconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

static void stream(int fd, void *data, size_t size, bool send_data)
{
    uint8_t *p = data;
    while (size) {
        ssize_t n = send_data ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n;
        size -= (size_t)n;
    }
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text), consumed;
    assert(!boxrig_terminal_feed(terminal, (const uint8_t *)text, length, &consumed));
    assert(consumed == length);
}

static void populate(boxrig_terminal *terminal, size_t history_rows, bool image)
{
    /* Fill both typical terminal text and a complete, uniquely numbered history. */
    for (size_t n = 0; n < history_rows + rows; ++n) {
        char line[80];
        snprintf(line, sizeof(line), "%05zu terminal output with a short line%s",
            n, n + 1 == history_rows + rows ? "" : "\r\n");
        feed(terminal, line);
    }
    assert(boxrig_terminal_scrollback_rows(terminal) == history_rows);
    if (image) {
        /* 512 x 384 opaque white RGBA; carry 3072 bytes per Kitty chunk. */
        char chunk[4097];
        memset(chunk, '/', sizeof(chunk) - 1);
        chunk[sizeof(chunk) - 1] = '\0';
        feed(terminal, "\033[1;1H");
        for (int i = 0; i < 256; ++i) {
            feed(terminal, i == 0 ? "\033_Ga=T,i=1,f=32,s=512,v=384,c=12,r=8,C=1,q=2,m=1;"
                          : i == 255 ? "\033_Gm=0;" : "\033_Gm=1;");
            feed(terminal, chunk);
            feed(terminal, "\033\\");
        }
        assert(boxrig_terminal_image_count(terminal) == 1);
    }
}

typedef struct draw_result {
    double received, imported, drawn;
} draw_result;

static void receive_and_draw(int socket_fd, size_t history_rows, bool image)
{
    /* Matching binaries share the local timing record and length representation. */
    size_t length;
    stream(socket_fd, &length, sizeof(length), false);
    assert(length && length <= wire_limit);
    uint8_t *data = malloc(length);
    assert(data);
    stream(socket_fd, data, length, false);
    draw_result times;
    times.received = milliseconds();
    boxrig_terminal *terminal;
    assert(!boxrig_terminal_import_checkpoint(data, length, wire_limit, &terminal));
    free(data);
    times.imported = milliseconds();
    assert(boxrig_terminal_scrollback_rows(terminal) == history_rows);
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_pango_cairo_renderer *renderer;
    boxrig_ui_view_config config = boxrig_ui_view_config_default();
    assert(!boxrig_ui_model_create(terminal, &model));
    assert(!boxrig_ui_view_create(model, &config, &view));
    assert(!boxrig_pango_cairo_renderer_create(columns * 7, rows * 14, &renderer));
    assert(!boxrig_pango_cairo_renderer_bind_view(renderer, view));
    assert(!boxrig_ui_model_synchronize(model));
    assert(!boxrig_pango_cairo_renderer_draw_view(renderer, view));
    cairo_surface_flush(boxrig_pango_cairo_renderer_surface(renderer));
    times.drawn = milliseconds();
    boxrig_pango_cairo_render_statistics work;
    assert(!boxrig_pango_cairo_renderer_get_render_statistics(renderer, &work));
    assert(work.glyph_draw_count && (!image || work.image_composite_count));
    stream(socket_fd, &times, sizeof(times), true);
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
}

static void measure(int run, size_t history_rows, bool image)
{
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(sockets[0]);
        receive_and_draw(sockets[1], history_rows, image);
        close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = rows;
    config.columns = columns;
    config.scrollback_capacity = history_limit;
    boxrig_terminal *terminal;
    assert(!boxrig_terminal_create(&config, &terminal));
    assert(!boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14));
    populate(terminal, history_rows, image);
    uint8_t *data;
    size_t size;
    double start = milliseconds();
    assert(!boxrig_terminal_export_checkpoint(terminal, wire_limit, &data, &size));
    double exported = milliseconds();
    stream(sockets[0], &size, sizeof(size), true);
    stream(sockets[0], data, size, true);
    draw_result times;
    stream(sockets[0], &times, sizeof(times), false);
    double resumed = milliseconds();
    printf("%d\t%zu\t%d\t%zu\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\n",
        run, history_rows, image ? 786432 : 0, size, exported - start,
        times.received - exported, times.imported - times.received,
        times.drawn - times.imported, resumed - start);
    fflush(stdout);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(sockets[0]);
    boxrig_checkpoint_free(data);
    boxrig_terminal_destroy(terminal);
}

int main(void)
{
    puts("run\thistory\timage_bytes\twire_bytes\texport_ms\tipc_ms\timport_ms\tfirst_draw_ms\tpause_ms");
    fflush(stdout);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) {
            int scenario = (i + j) % 4;
            measure(i + 1, scenario & 1 ? history_limit : 0, (scenario & 2) != 0);
        }
    return 0;
}
