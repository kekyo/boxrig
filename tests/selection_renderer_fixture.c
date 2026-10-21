/* boxrig - Under MIT. */
#include "selection_renderer_fixture.h"
#include "blink_renderer_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef struct selection_filter {
    size_t calls;
    bool selection_cell;
    bool selection_marker;
    boxrig_rgb marker_underlay;
} selection_filter;

static boxrig_rgb shift_color(boxrig_rgb color)
{
    return (boxrig_rgb){color.green, color.blue, color.red};
}

static boxrig_ui_color_filter_result inspect_selection(
    const boxrig_ui_color_filter_input *input, void *context)
{
    selection_filter *seen = context;
    seen->calls++;
    if (input->target == BOXRIG_UI_COLOR_CELL &&
        (input->state_flags & BOXRIG_UI_COLOR_SELECTED) != 0 &&
        input->background_source.role == BOXRIG_UI_COLOR_UI) {
        seen->selection_cell = true;
    }
    if (input->target == BOXRIG_UI_COLOR_SELECTION_MARKER) {
        seen->selection_marker = input->marker_source.role == BOXRIG_UI_COLOR_UI &&
            input->underlay_valid;
        seen->marker_underlay = input->underlay.background;
    }
    boxrig_ui_color_filter_result result = input->colors;
    result.background = shift_color(result.background);
    result.foreground = shift_color(result.foreground);
    result.underline = shift_color(result.underline);
    result.marker = shift_color(result.marker);
    return result;
}

static uint32_t rgb(boxrig_rgb color)
{
    return ((uint32_t)color.red << 16) | ((uint32_t)color.green << 8) | color.blue;
}

static uint32_t over(uint32_t background, uint32_t foreground, unsigned alpha)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned base = (background >> shift) & 255;
        unsigned top = (foreground >> shift) & 255;
        result |= ((base * (255-alpha) + top * alpha + 127) / 255) << shift;
    }
    return result;
}

static bool close_rgb(uint32_t actual, uint32_t expected)
{
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        int delta = (int)((actual >> shift) & 255) - (int)((expected >> shift) & 255);
        if (delta < -2 || delta > 2) return false;
    }
    return true;
}

static bool selection_history_and_ime(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 12;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    view_config.blink.cursor_enabled = false;
    view_config.blink.text_enabled = false;
    boxrig_test_color_renderer renderer = {0};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    const unsigned width = renderer.cell_width, height = renderer.cell_height;
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 12*width*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 4*height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = width*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const char text[] = "\033[?25l\033[48;2;16;32;48m\033[2J\033[H"
        "        \r\n        \r\n        \r\n        \r\n        \r\n        \r\n";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) >= 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    boxrig_ui_hit_test_result first, last;
    CHECK(boxrig_ui_view_hit_test(view, (width+1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (7*width-1)*BOXRIG_UI_FIXED_ONE,
        (2*height+1)*BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    boxrig_ui_selection_options options = {BOXRIG_UI_SELECTION_RECTANGULAR,
        BOXRIG_UI_SELECTION_CHARACTER};
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    boxrig_text_snapshot *before = NULL, *after = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &before) == BOXRIG_UI_OK);
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    style.presentation = BOXRIG_UI_SELECTION_FILL;
    style.background = (boxrig_rgb){100,180,220};
    style.background_opacity = 128;
    CHECK(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (unsigned y = 0; y < 4*height; y++) for (unsigned x = 0; x < 12*width; x++) {
        uint32_t expected = 0x102030u;
        if (y < 3*height && x >= width && x < 7*width)
            expected = over(expected, rgb(style.background), 128);
        CHECK(close_rgb(pixels[y*BOXRIG_TEST_COLOR_WIDTH+x], expected));
    }
    free(pixels);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &after) == BOXRIG_UI_OK);
    size_t before_count, after_count;
    const uint8_t *before_text = boxrig_text_snapshot_data(before, &before_count);
    const uint8_t *after_text = boxrig_text_snapshot_data(after, &after_count);
    CHECK(before_count != 0 && before_count == after_count &&
        memcmp(before_text, after_text, before_count) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    boxrig_text_snapshot_destroy(before); boxrig_text_snapshot_destroy(after);

    /* Empty geometry erases the old fill; history expansion paints only the
     * visible part even when its anchor is outside the viewport. */
    CHECK(boxrig_ui_view_hit_test(view, (width+1)*BOXRIG_UI_FIXED_ONE,
        (height+1)*BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (unsigned y = 0; y < 4*height; y++) for (unsigned x = 0; x < 12*width; x++)
        CHECK(close_rgb(pixels[y*BOXRIG_TEST_COLOR_WIDTH+x], 0x102030u));
    free(pixels);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(boxrig_ui_view_hit_test(view, (7*width-1)*BOXRIG_UI_FIXED_ONE,
        (3*height+1)*BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    boxrig_ui_selection history;
    CHECK(boxrig_ui_view_get_selection(view, &history) && history.row_count == 6 &&
        !history.anchor_visual_row_valid);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (unsigned y = 0; y < 4*height; y++) for (unsigned x = 0; x < 12*width; x++) {
        uint32_t expected = x >= width && x < 7*width
            ? over(0x102030u, rgb(style.background), 128) : 0x102030u;
        CHECK(close_rgb(pixels[y*BOXRIG_TEST_COLOR_WIDTH+x], expected));
    }
    free(pixels);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (unsigned y = 0; y < 4*height; y++) for (unsigned x = 0; x < 12*width; x++)
        CHECK(close_rgb(pixels[y*BOXRIG_TEST_COLOR_WIDTH+x], 0x102030u));
    free(pixels);

    /* Preedit owns its foreground/background above the selected terminal cells.
     * A style-only redraw must preserve it and reveal the new style on cancel. */
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    const char home[] = "\033[1;3H";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)home, sizeof(home)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(boxrig_ui_view_hit_test(view, (width+1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (7*width-1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    boxrig_composition *composition = NULL;
    CHECK(boxrig_composition_create(terminal, boxrig_composition_limits_default(),
        &composition) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_segment segment = {0, 2, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE};
    boxrig_composition_event event = {0};
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = "  "; event.text_length = 2;
    event.segments = &segment; event.segment_count = 1;
    event.selection_length = 1; event.caret_offset = 1; event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    uint32_t *preedit = renderer.capture(renderer.context);
    CHECK(preedit != NULL);
    style.presentation = BOXRIG_UI_SELECTION_FILL_OUTLINE;
    style.background = (boxrig_rgb){220,120,80};
    style.marker_opacity = 160;
    CHECK(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (unsigned y = 0; y < height; y++) for (unsigned x = 2*width; x < 4*width; x++)
        CHECK(pixels[y*BOXRIG_TEST_COLOR_WIDTH+x] == preedit[y*BOXRIG_TEST_COLOR_WIDTH+x]);
    CHECK(memcmp(pixels, preedit, BOXRIG_TEST_COLOR_WIDTH*BOXRIG_TEST_COLOR_HEIGHT*sizeof(*pixels)) != 0);
    free(pixels); free(preedit);
    event = (boxrig_composition_event){0};
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(close_rgb(pixels[(height/2)*BOXRIG_TEST_COLOR_WIDTH+2*width+width/2],
        over(0x102030u, rgb(style.background), 128)));
    free(pixels);
    boxrig_composition_destroy(composition);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS rectangular history selection, immutable copy and IME overlap/cancel");
    return true;
}

static bool selection_unvisited_unicode(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_test_color_renderer renderer = {0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 12; config.scrollback_capacity = 128;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t consumed;
    const char text[] = "Aאב界e\xcc\x81😀Z\r\n";
    for (size_t row = 0; row < 70; row++)
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
            sizeof(text) - (row == 69 ? 3 : 1), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    unsigned width = renderer.cell_width, height = renderer.cell_height;
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 12*width*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 4*height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = width*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const struct { unsigned first, last; const char *text; } cases[] = {
        {1, 2, "ב"}, {3, 4, "界"}, {5, 6, "e\xcc\x81"},
        {6, 7, "😀"}, {1, 8, "אב界e\xcc\x81😀"}, {9, 12, "   "}
    };
    boxrig_ui_selection_options options = {BOXRIG_UI_SELECTION_RECTANGULAR,
        BOXRIG_UI_SELECTION_CHARACTER};
    for (size_t test = 0; test < sizeof(cases)/sizeof(cases[0]); test++) {
        CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
        CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        boxrig_ui_hit_test_result hit;
        CHECK(boxrig_ui_view_hit_test(view, (cases[test].first*width+1)*BOXRIG_UI_FIXED_ONE,
            (3*height+1)*BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
        CHECK(boxrig_terminal_set_viewport_offset(terminal, 3) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        CHECK(boxrig_ui_view_hit_test(view, (cases[test].last*width-1)*BOXRIG_UI_FIXED_ONE,
            BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
        boxrig_ui_selection selection;
        CHECK(boxrig_ui_view_get_selection(view, &selection) && !selection.stale && selection.row_count == 7);
        boxrig_text_snapshot *copy = NULL;
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
        size_t count;
        const uint8_t *bytes = boxrig_text_snapshot_data(copy, &count);
        size_t length = strlen(cases[test].text);
        CHECK(count == 7*length+6);
        for (size_t row = 0; row < 7; row++) {
            CHECK(memcmp(bytes+row*(length+1), cases[test].text, length) == 0);
            if (row != 6) CHECK(bytes[row*(length+1)+length] == '\n');
        }
        boxrig_text_snapshot_destroy(copy);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        CHECK(boxrig_ui_view_get_selection(view, &selection) && !selection.stale);
    }
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS unvisited history: BiDi, wide/combining/emoji clusters and blank padding");
    return true;
}

bool boxrig_test_renderer_selection_style(
    bool (*create)(boxrig_test_color_renderer *renderer),
    unsigned int expected_stroke_x, unsigned int expected_stroke_y)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 12;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    view_config.blink.cursor_enabled = false;
    view_config.blink.text_enabled = false;
    boxrig_test_color_renderer renderer = {0};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    const unsigned width = renderer.cell_width, height = renderer.cell_height;
    CHECK(12*width <= BOXRIG_TEST_COLOR_WIDTH && 4*height <= BOXRIG_TEST_COLOR_HEIGHT);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 12*width*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 4*height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = width*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    /* A blank separator keeps neighboring glyph overhang out of the alpha fixture. */
    const char text[] = "\033[?25l\033[48;2;16;32;48m\033[2J"
        "\033[3;1HX\xe7\x95\x8c" "e\xcc\x88 fi \327\220\327\221\327\222"
        "\033[3;12H\033[8mX\033[28m";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    boxrig_ui_hit_test_result first, last;
    CHECK(boxrig_ui_view_hit_test(view, (width+1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (7*width-1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    boxrig_text_layout_range ranges[12];
    CHECK(boxrig_ui_view_selection_visual_ranges(view, 0, ranges, 12) == 1);
    CHECK(ranges[0].column == 1 && ranges[0].column_count == 6);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *original = renderer.capture(renderer.context);
    CHECK(original != NULL);
    renderer.reset_statistics(renderer.context);
    selection_filter seen = {0};
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    style.marker = (boxrig_rgb){220, 100, 60};
    style.background = (boxrig_rgb){120, 180, 240};
    style.line_width_dip = 3;
    for (unsigned filtered = 0; filtered < 2; filtered++) {
        CHECK(boxrig_ui_view_set_color_filter(view,
            filtered ? inspect_selection : NULL, filtered ? &seen : NULL) == BOXRIG_UI_OK);
        uint32_t base = rgb(filtered ? shift_color((boxrig_rgb){16,32,48}) : (boxrig_rgb){16,32,48});
        uint32_t fill = rgb(filtered ? shift_color(style.background) : style.background);
        uint32_t marker = rgb(filtered ? shift_color(style.marker) : style.marker);
        for (int mode = 0; mode < 3; mode++) for (unsigned a = 0; a < 4; a++) {
            style.presentation = (boxrig_ui_selection_presentation)mode;
            style.background_opacity = (uint8_t[]){0,64,128,255}[a];
            style.marker_opacity = (uint8_t[]){0,128,255,64}[a];
            CHECK(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            uint32_t *pixels = renderer.capture(renderer.context);
            CHECK(pixels != NULL);
            for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < 12*width; x++) {
                uint32_t expected = base;
                bool selected = x >= width && x < 7*width;
                if (selected && mode != BOXRIG_UI_SELECTION_OUTLINE)
                    expected = over(expected, fill, style.background_opacity);
                bool perimeter = y < expected_stroke_y || y >= height-expected_stroke_y ||
                    x < width+expected_stroke_x || x >= 7*width-expected_stroke_x;
                if (selected && perimeter && mode != BOXRIG_UI_SELECTION_FILL)
                    expected = over(expected, marker, style.marker_opacity);
                uint32_t actual = pixels[y*BOXRIG_TEST_COLOR_WIDTH+x];
                if (!close_rgb(actual, expected)) {
                    fprintf(stderr, "selection pixel filtered=%u mode=%d alpha=%u x=%u y=%u actual=%06x expected=%06x\n",
                        filtered, mode, a, x, y, actual, expected);
                    CHECK(false);
                }
            }
            free(pixels);
        }
    }
    CHECK(seen.selection_cell && seen.selection_marker);
    CHECK(boxrig_ui_view_set_color_filter(view, NULL, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_reset_selection_style(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *restored = renderer.capture(renderer.context);
    CHECK(restored != NULL && memcmp(original, restored,
        BOXRIG_TEST_COLOR_WIDTH*BOXRIG_TEST_COLOR_HEIGHT*sizeof(*restored)) == 0);
    free(restored); free(original);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);

    /* Selection colors change visible glyphs, including wide, combining and
     * bidi text, while copy text and concealed cells retain their meaning. */
    CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
        (2*height+1)*BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (12*width-1)*BOXRIG_UI_FIXED_ONE,
        (2*height+1)*BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    boxrig_text_snapshot *before = NULL, *after = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &before) == BOXRIG_UI_OK);
    style.presentation = BOXRIG_UI_SELECTION_FILL;
    style.background_opacity = 0;
    style.foreground_policy = BOXRIG_UI_SELECTION_FOREGROUND_RGB;
    style.foreground = (boxrig_rgb){25,90,230};
    CHECK(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *foreground = renderer.capture(renderer.context);
    CHECK(foreground != NULL);
    size_t painted_glyphs = 0;
    for (unsigned y = 2*height; y < 3*height; y++) for (unsigned x = 0; x < 12*width; x++) {
        uint32_t actual = foreground[y*BOXRIG_TEST_COLOR_WIDTH+x];
        if (actual != 0x102030u) {
            /* Small antialiased glyphs need not contain a fully covered pixel.
             * Every coverage sample must still mix the requested foreground
             * with the unchanged cell background, never the old foreground. */
            unsigned r = (actual >> 16) & 255, g = (actual >> 8) & 255, b = actual & 255;
            if (!(r >= 14 && r <= 27 && g >= 30 && g <= 92 && b >= 46 && b <= 232))
                fprintf(stderr, "selection ink x=%u y=%u actual=%06x\n", x, y, actual);
            CHECK(r >= 14 && r <= 27 && g >= 30 && g <= 92 && b >= 46 && b <= 232);
            painted_glyphs++;
        }
        if (x >= 11*width) CHECK(actual == 0x102030u);
    }
    CHECK(painted_glyphs != 0);
    free(foreground);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &after) == BOXRIG_UI_OK);
    size_t before_count, after_count;
    const uint8_t *before_text = boxrig_text_snapshot_data(before, &before_count);
    const uint8_t *after_text = boxrig_text_snapshot_data(after, &after_count);
    CHECK(before_count > 0 && before_count == after_count &&
        memcmp(before_text, after_text, before_count) == 0);
    boxrig_text_snapshot_destroy(before); boxrig_text_snapshot_destroy(after);
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);

    /* Background images receive fill alpha. Foreground images cover the fill
     * and glyphs, while the perimeter is consistently above both image layers. */
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, width, height) == BOXRIG_STATUS_OK);
    const char images[] = "\033[1;2H\033_Ga=T,f=32,s=1,v=1,c=2,r=1,z=-1,C=1;AP8A/w==\033\\"
        "\033[1;5H\033_Ga=T,f=32,s=1,v=1,c=2,r=1,z=1,C=1;/wAA/w==\033\\";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)images, sizeof(images)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (width+1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (7*width-1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    style.presentation = BOXRIG_UI_SELECTION_FILL_OUTLINE;
    style.foreground_policy = BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL;
    style.marker_opacity = 128; style.background_opacity = 128;
    CHECK(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *layered = renderer.capture(renderer.context);
    CHECK(layered != NULL);
    for (unsigned y = 0; y < height; y++) for (unsigned x = width; x < 7*width; x++) {
        uint32_t base = x < 3*width ? 0x00ff00u : 0x102030u;
        uint32_t expected = over(base, rgb(style.background), 128);
        if (x >= 4*width && x < 6*width) expected = 0xff0000u;
        bool perimeter = y < expected_stroke_y || y >= height-expected_stroke_y ||
            x < width+expected_stroke_x || x >= 7*width-expected_stroke_x;
        if (perimeter) expected = over(expected, rgb(style.marker), 128);
        if (!close_rgb(layered[y*BOXRIG_TEST_COLOR_WIDTH+x], expected)) {
            fprintf(stderr, "selection/image pixel x=%u y=%u actual=%06x expected=%06x\n",
                x, y, layered[y*BOXRIG_TEST_COLOR_WIDTH+x], expected);
            CHECK(false);
        }
    }
    free(layered);
    CHECK(boxrig_ui_view_set_color_filter(view, inspect_selection, &seen) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(close_rgb(rgb(seen.marker_underlay), over(rgb(shift_color((boxrig_rgb){16,32,48})),
        rgb(shift_color(style.background)), 128)));
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(selection_history_and_ime(create));
    CHECK(selection_unvisited_unicode(create));
    CHECK(boxrig_test_renderer_selection_blink(create));
    puts("PASS selection styles, alpha, filtering, DPI, retained glyphs, wide/BiDi copy and image layers");
    return true;
}
