/* boxrig - Under MIT. Measure selection work at retained-history scales. */
#define _POSIX_C_SOURCE 200809L
#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"
#include "../../src/ui/view_private.h"
#ifdef BOXRIG_SELECTION_COST_CAIRO
#include "../../src/pangocairo/renderer.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static size_t fragment_reads;
static size_t snapshot_creations;

boxrig_status __real_boxrig_terminal_get_text_fragment(
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t index, boxrig_text_fragment_view *fragment);

boxrig_status __wrap_boxrig_terminal_get_text_fragment(
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t index, boxrig_text_fragment_view *fragment)
{
    fragment_reads++;
    return __real_boxrig_terminal_get_text_fragment(terminal, screen,
        index, fragment);
}

boxrig_status __real_boxrig_terminal_create_text_snapshot(
    const boxrig_terminal *terminal, const boxrig_text_snapshot_options *options,
    boxrig_text_snapshot **snapshot_out);

boxrig_status __wrap_boxrig_terminal_create_text_snapshot(
    const boxrig_terminal *terminal, const boxrig_text_snapshot_options *options,
    boxrig_text_snapshot **snapshot_out)
{
    snapshot_creations++;
    return __real_boxrig_terminal_create_text_snapshot(terminal, options,
        snapshot_out);
}

static void require(bool condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", description);
        exit(EXIT_FAILURE);
    }
}

static uint64_t clock_ns(void)
{
    struct timespec now;
    require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static void feed(boxrig_terminal *terminal, const char *text, size_t length)
{
    size_t consumed = 0;
    require(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        length, &consumed) == BOXRIG_STATUS_OK && consumed == length,
        "feed history");
}

static void run_case(size_t history, bool wrapped, size_t repetitions)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
#ifdef BOXRIG_SELECTION_COST_CAIRO
    boxrig_pango_cairo_renderer *renderer = NULL;
#endif
    char line[200];
    size_t length;
    config.rows = 4;
    config.columns = 80;
    config.scrollback_capacity = history;
    require(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK,
        "create terminal");
    if (wrapped) {
        memset(line, 'A', 160);
        memcpy(line + 160, "\r\n", 2);
        length = 162;
    } else {
        memcpy(line, "short line\r\n", 12);
        length = 12;
    }
    /* Build enough history without keeping any measurement counters active. */
    size_t lines = wrapped ? (history + config.rows + 2) / 2 :
        history + config.rows - 1;
    for (size_t index = 0; index < lines; index++) {
        feed(terminal, line, length);
    }
    require(boxrig_terminal_scrollback_rows(terminal) == history,
        "retain requested history");
    require(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK,
        "create model");
    require(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK,
        "create view");
#ifdef BOXRIG_SELECTION_COST_CAIRO
    require(boxrig_pango_cairo_renderer_create(800, 80, &renderer) == 0,
        "create Cairo renderer");
    require(boxrig_pango_cairo_renderer_bind_view(renderer, view) == BOXRIG_UI_OK,
        "bind Cairo renderer");
#endif
    require(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK,
        "acquire layout");
    boxrig_ui_allocation_input allocation = {0};
#ifdef BOXRIG_SELECTION_COST_CAIRO
    unsigned int cell_width = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int cell_height = boxrig_pango_cairo_renderer_cell_height(renderer);
    require(boxrig_pango_cairo_renderer_set_surface_size(renderer,
        80 * cell_width, 4 * cell_height) == 0, "size Cairo surface");
#else
    unsigned int cell_width = 8, cell_height = 10;
#endif
    allocation.allocation_width = 80 * cell_width * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 4 * cell_height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = cell_width * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = cell_height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    boxrig_ui_layout_result layout;
    require(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
        BOXRIG_UI_OK, "allocate view");
    require(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK,
        "synchronize model");
#ifdef BOXRIG_SELECTION_COST_CAIRO
    require(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0,
        "initial Cairo frame");
#endif
    boxrig_ui_hit_test_result anchor, near, far;
    require(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK &&
        boxrig_ui_view_hit_test(view, cell_width * BOXRIG_UI_FIXED_ONE,
        (cell_height + 1) * BOXRIG_UI_FIXED_ONE, &near) == BOXRIG_UI_OK &&
        boxrig_ui_view_hit_test(view, 3 * cell_width * BOXRIG_UI_FIXED_ONE,
        (cell_height + 1) * BOXRIG_UI_FIXED_ONE, &far) == BOXRIG_UI_OK,
        "hit visible rows");
    require(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER,
        &anchor) == BOXRIG_UI_OK, "begin selection");
    size_t update_reads = 0, draw_reads = 0, update_snapshots = 0;
    size_t draw_snapshots = 0, ranges = 0;
    size_t raster_reads = 0, raster_snapshots = 0;
    uint64_t update_ns = 0, draw_ns = 0;
    uint64_t raster_ns = 0;
    for (size_t iteration = 0; iteration < repetitions; iteration++) {
        fragment_reads = 0;
        snapshot_creations = 0;
        uint64_t start = clock_ns();
        require(boxrig_ui_view_update_selection(view,
            iteration % 2 ? &near : &far) == BOXRIG_UI_OK,
            "update selection");
        update_ns += clock_ns() - start;
        update_reads += fragment_reads;
        update_snapshots += snapshot_creations;
        fragment_reads = 0;
        snapshot_creations = 0;
        start = clock_ns();
        for (size_t row = 0; row < 4; row++) {
            boxrig_text_layout_range visual[80];
            ranges += boxrig_ui_view_selection_visual_ranges(view,
                row, visual, 80);
            for (size_t column = 0; column < 80; column++) {
                boxrig_ui_color_filter_input color = {0};
                boxrig_ui_view_apply_color_state(view, row, column,
                    0, false, &color);
            }
        }
        draw_ns += clock_ns() - start;
        draw_reads += fragment_reads;
        draw_snapshots += snapshot_creations;
#ifdef BOXRIG_SELECTION_COST_CAIRO
        fragment_reads = 0;
        snapshot_creations = 0;
        start = clock_ns();
        require(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0,
            "redraw Cairo frame");
        raster_ns += clock_ns() - start;
        raster_reads += fragment_reads;
        raster_snapshots += snapshot_creations;
#endif
    }
    require(update_snapshots == 0 && draw_snapshots == 0 &&
        raster_snapshots == 0,
        "selection work creates no full snapshot");
    printf("MEASURE selection history=%zu shape=%s iterations=%zu "
        "update_ns=%" PRIu64 " redraw_prepare_ns=%" PRIu64
        " raster_ns=%" PRIu64 " update_fragments=%zu redraw_fragments=%zu "
        "raster_fragments=%zu update_snapshots=%zu redraw_snapshots=%zu "
        "raster_snapshots=%zu ranges=%zu\n",
        history, wrapped ? "wrapped" : "short", repetitions,
        update_ns, draw_ns, raster_ns, update_reads, draw_reads,
        raster_reads, update_snapshots, draw_snapshots, raster_snapshots,
        ranges);
#ifdef BOXRIG_SELECTION_COST_CAIRO
    boxrig_pango_cairo_renderer_destroy(renderer);
#endif
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
}

int main(int argc, char **argv)
{
    require(argc == 1 || (argc == 2 && strcmp(argv[1], "measure") == 0),
        "usage: selection_cost [measure]");
    size_t repetitions = argc == 1 ? 8 : 100;
    for (size_t shape = 0; shape < 2; shape++) {
        for (size_t history = 0; history <= 10000;
                history = history == 0 ? 1000 : 10000) {
            run_case(history, shape != 0, repetitions);
            if (history == 10000) break;
        }
    }
    return EXIT_SUCCESS;
}
