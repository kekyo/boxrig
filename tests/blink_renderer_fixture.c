/* boxrig - Under MIT. */
#include "blink_renderer_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cursor_colors {
    size_t calls;
    boxrig_ui_color_source_role source;
    boxrig_cursor_shape shape;
} cursor_colors;

static boxrig_ui_color_filter_result inspect_cursor_color(
    const boxrig_ui_color_filter_input *input, void *context)
{
    cursor_colors *seen = context;
    if (input->target == BOXRIG_UI_COLOR_CURSOR) {
        seen->calls++;
        seen->source = input->marker_source.role;
        seen->shape = input->cursor_shape;
    }
    return input->colors;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static boxrig_ui_color_filter_result count_colors(
    const boxrig_ui_color_filter_input *input, void *opaque)
{
    size_t *calls = opaque;
    ++*calls;
    return boxrig_ui_color_filter_result_default(input);
}

bool boxrig_test_renderer_cursor_blink(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_test_color_renderer renderer = {0};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    config.rows = 2;
    config.columns = 12;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    size_t calls = 0;
    CHECK(boxrig_ui_view_set_color_filter(view, count_colors, &calls) == BOXRIG_UI_OK);
    const char text[] = "L R\033[1;2H\033[1 q";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_visible(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *on = renderer.capture(renderer.context);
    CHECK(on != NULL);
    const size_t count = BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT;
    size_t initial_calls = calls;
    renderer.reset_statistics(renderer.context);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_cursor_blink_visible(view));
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *off = renderer.capture(renderer.context);
    CHECK(off != NULL);
    size_t changed = 0;
    for (size_t index = 0; index < count; index++) {
        if (on[index] != off[index]) {
            size_t x = index % BOXRIG_TEST_COLOR_WIDTH;
            size_t y = index / BOXRIG_TEST_COLOR_WIDTH;
            CHECK(y < renderer.cell_height && x >= renderer.cell_width &&
                x < renderer.cell_width * 2u);
            changed++;
        }
    }
    CHECK(changed != 0);
    free(off);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 1000) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *again = renderer.capture(renderer.context);
    CHECK(again != NULL && memcmp(on, again, count * sizeof(*on)) == 0);
    free(again);
    CHECK(calls == initial_calls);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);

    CHECK(boxrig_ui_view_update_monotonic_time(view, 1500) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(boxrig_ui_view_notify_local_input(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    again = renderer.capture(renderer.context);
    CHECK(again != NULL && memcmp(on, again, count * sizeof(*on)) == 0);
    free(again);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 2000) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    again = renderer.capture(renderer.context);
    CHECK(again != NULL && memcmp(on, again, count * sizeof(*on)) == 0);
    free(again);
    uint64_t deadline;
    CHECK(!boxrig_ui_view_next_animation_deadline(view, &deadline));

    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[2 q", 5,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 2500) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    again = renderer.capture(renderer.context);
    CHECK(again != NULL && memcmp(on, again, count * sizeof(*on)) == 0);
    free(again);
    CHECK(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    free(on);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

bool boxrig_test_renderer_text_blink(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_test_color_renderer renderer = {0};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    config.rows = 2;
    config.columns = 12;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    size_t calls = 0;
    CHECK(boxrig_ui_view_set_color_filter(view, count_colors, &calls) == BOXRIG_UI_OK);
    const char text[] = "\033[48;2;17;34;51m\033[2J\033[1;2H"
        "\033[5;4;9mA\033[1;5H\033[6mB\033[1;8H\033[25mC"
        "\033[1;10H\033[5;8mD\033[0m\033[2;3H\033[1 q";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *on = renderer.capture(renderer.context);
    CHECK(on != NULL);
    const size_t count = BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT;
    size_t initial_calls = calls;
    renderer.reset_statistics(renderer.context);
    uint32_t *off_reference = NULL;
    const uint64_t times[] = {150, 500, 1050, 1200};
    const bool slow_visible[] = {true, false, true, true};
    const bool rapid_visible[] = {false, false, false, true};
    const bool cursor_visible[] = {true, false, true, true};
    for (size_t phase = 0; phase < 4; phase++) {
        CHECK(boxrig_ui_view_update_monotonic_time(view, times[phase]) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *pixels = renderer.capture(renderer.context);
        CHECK(pixels != NULL);
        size_t changed[3] = {0};
        for (size_t index = 0; index < count; index++) {
            size_t x = index % BOXRIG_TEST_COLOR_WIDTH;
            size_t y = index / BOXRIG_TEST_COLOR_WIDTH;
            int target = -1;
            if (y < renderer.cell_height && x / renderer.cell_width == 1) target = 0;
            if (y < renderer.cell_height && x / renderer.cell_width == 4) target = 1;
            if (y / renderer.cell_height == 1 && x / renderer.cell_width == 2) target = 2;
            if (pixels[index] != on[index]) {
                if (target < 0) fprintf(stderr, "outside phase=%zu xy=%zu,%zu on=%06x current=%06x\n",phase,x,y,on[index],pixels[index]);
                CHECK(target >= 0);
                changed[target]++;
            }
            if (target == 0 && !slow_visible[phase]) CHECK(pixels[index] == 0x112233u);
            if (target == 1 && !rapid_visible[phase]) {
                if (pixels[index] != 0x112233u) fprintf(stderr, "phase=%zu xy=%zu,%zu pixel=%06x gate=%d\n", phase,x,y,pixels[index],boxrig_ui_view_text_blink_visible(view,BOXRIG_CELL_BLINK|BOXRIG_CELL_RAPID_BLINK));
                CHECK(pixels[index] == 0x112233u);
            }
        }
        CHECK((changed[0] == 0) == slow_visible[phase]);
        CHECK((changed[1] == 0) == rapid_visible[phase]);
        CHECK((changed[2] == 0) == cursor_visible[phase]);
        if (phase == 1) off_reference = pixels;
        else free(pixels);
    }
    for (uint64_t cycle = 1; cycle <= 16; cycle++) {
        for (unsigned phase = 0; phase < 2; phase++) {
            CHECK(boxrig_ui_view_update_monotonic_time(view, cycle * 3000 + phase * 500) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            uint32_t *pixels = renderer.capture(renderer.context);
            CHECK(pixels != NULL);
            const uint32_t *expected = phase == 0 ? on : off_reference;
            for (size_t index = 0; index < count; index++) {
                if (pixels[index] != expected[index]) fprintf(stderr,"cycle=%llu phase=%u xy=%zu,%zu expected=%06x actual=%06x\n",(unsigned long long)cycle,phase,index % BOXRIG_TEST_COLOR_WIDTH,index / BOXRIG_TEST_COLOR_WIDTH,expected[index],pixels[index]);
                CHECK(pixels[index] == expected[index]);
            }
            free(pixels);
        }
    }
    free(off_reference);
    CHECK(calls == initial_calls);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
    free(on);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

bool boxrig_test_renderer_cursor_style_at_scale(
    bool (*create)(boxrig_test_color_renderer *renderer),
    unsigned int expected_stroke_x, unsigned int expected_stroke_y)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_test_color_renderer renderer = {0};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    config.rows = 1;
    config.columns = 8;
    view_config.blink.cursor_enabled = false;
    view_config.blink.text_enabled = false;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t consumed = 0;
    const char *text = "AB\033[2 q\033[?25l";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    cursor_colors seen = {0};
    CHECK(boxrig_ui_view_set_color_filter(view, inspect_cursor_color, &seen) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *underlay = renderer.capture(renderer.context);
    CHECK(underlay != NULL);
    const char *show = "\033[?25h";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)show, strlen(show),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *original = renderer.capture(renderer.context);
    CHECK(original != NULL);
    CHECK(seen.calls != 0 && seen.source == BOXRIG_UI_COLOR_DEFAULT_CURSOR);
    renderer.reset_statistics(renderer.context);
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BAR;
    style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
    style.color = (boxrig_rgb){241, 35, 19};
    style.line_width_dip = 3;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *bar = renderer.capture(renderer.context);
    CHECK(bar != NULL);
    const unsigned width = renderer.cell_width, height = renderer.cell_height;
    size_t colored = 0;
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width * 8; x++) {
        if (bar[y * BOXRIG_TEST_COLOR_WIDTH + x] == 0xf12313u) {
            CHECK(x >= 2 * width && x < 3 * width);
            colored++;
        }
    }
    CHECK(colored == expected_stroke_x * height);
    CHECK(seen.source == BOXRIG_UI_COLOR_UI && seen.shape == BOXRIG_CURSOR_BAR);
    const char *override = "\033]12;#010203\007\033[4 q";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)override, strlen(override),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_cursor_view logical;
    boxrig_terminal_get_cursor(terminal, &logical);
    CHECK(logical.shape == BOXRIG_CURSOR_UNDERLINE && !logical.blinking);
    boxrig_rgb logical_color;
    CHECK(boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_CURSOR,
        &logical_color) == BOXRIG_STATUS_OK);
    CHECK(logical_color.red == 1 && logical_color.green == 2 && logical_color.blue == 3);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *overridden = renderer.capture(renderer.context);
    CHECK(overridden != NULL && memcmp(overridden, bar,
        BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT * sizeof(*bar)) == 0);
    free(overridden);
    const char *move = "\033[1;5H";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)move, strlen(move),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *moved = renderer.capture(renderer.context);
    CHECK(moved != NULL);
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
        /* Compare each position with its own underlay: a neighboring glyph
         * may have antialiased ink beyond its nominal cell. */
        CHECK(moved[y * BOXRIG_TEST_COLOR_WIDTH + 2 * width + x] ==
            underlay[y * BOXRIG_TEST_COLOR_WIDTH + 2 * width + x]);
        uint32_t expected = bar[y * BOXRIG_TEST_COLOR_WIDTH + 2 * width + x] ==
            0xf12313u ? 0xf12313u :
            underlay[y * BOXRIG_TEST_COLOR_WIDTH + 4 * width + x];
        CHECK(moved[y * BOXRIG_TEST_COLOR_WIDTH + 4 * width + x] == expected);
    }
    free(moved);
    const char *restore = "\033]112\007\033[2 q\033[1;3H";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)restore, strlen(restore),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    free(bar);
    style.shape = BOXRIG_UI_CURSOR_SHAPE_UNDERLINE;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *underline = renderer.capture(renderer.context);
    CHECK(underline != NULL);
    colored = 0;
    for (unsigned y = 0; y < height; y++) for (unsigned x = 2 * width; x < 3 * width; x++)
        if (underline[y * BOXRIG_TEST_COLOR_WIDTH + x] == 0xf12313u) colored++;
    CHECK(colored == width * expected_stroke_y);
    free(underline);
    /* Box thickness and perimeter width scale independently. DECSCUSR still
     * requests a block, so the bar/underline must remain explicit overrides. */
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED;
    style.box_width_dip = 3;
    style.line_width_dip = 1;
    for (unsigned shape = 0; shape < 3; shape++) {
        style.shape = (boxrig_ui_cursor_shape_policy[]){BOXRIG_UI_CURSOR_SHAPE_BLOCK,
            BOXRIG_UI_CURSOR_SHAPE_BAR, BOXRIG_UI_CURSOR_SHAPE_UNDERLINE}[shape];
        CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
        for (unsigned focus = 0; focus < 2; focus++) {
            CHECK(boxrig_ui_view_set_focused(view, focus != 0) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            uint32_t *box = renderer.capture(renderer.context);
            CHECK(box != NULL);
            unsigned bw = shape == 1 ? expected_stroke_x : width;
            unsigned bh = shape == 2 ? expected_stroke_y : height;
            unsigned sx = (expected_stroke_x + 1) / 3;
            unsigned sy = (expected_stroke_y + 1) / 3;
            for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
                size_t at = y*BOXRIG_TEST_COLOR_WIDTH + 2*width+x;
                bool inside = x < bw && y >= height-bh;
                bool border = x < sx || x >= bw-sx || y < height-bh+sy || y >= height-sy;
                CHECK(box[at] == (inside && (focus || border) ? 0xf12313u : underlay[at]));
            }
            free(box);
        }
    }
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_DEFAULT;
    style.shape = BOXRIG_UI_CURSOR_SHAPE_OUTLINE;
    style.line_width_dip = 1000;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *clamped = renderer.capture(renderer.context);
    CHECK(clamped != NULL);
    for (unsigned y = 0; y < height; y++) for (unsigned x = 2 * width; x < 3 * width; x++)
        CHECK(clamped[y * BOXRIG_TEST_COLOR_WIDTH + x] == 0xf12313u);
    free(clamped);
    CHECK(boxrig_ui_view_reset_cursor_style(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *reset = renderer.capture(renderer.context);
    CHECK(reset != NULL && memcmp(reset, original,
        BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT * sizeof(*reset)) == 0);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
    free(reset);
    free(original);
    free(underlay);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool renders_blink_composition(
    bool (*create)(boxrig_test_color_renderer *renderer), bool selected_fill)
{
    const size_t count = BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT;
    uint32_t *hidden_blink = NULL;
    for (unsigned reference = 0; reference < 2; reference++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
        boxrig_test_color_renderer renderer = {0};
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_ui_view *view = NULL;
        config.rows = 2;
        config.columns = 12;
        CHECK(create(&renderer));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
        unsigned width = renderer.cell_width, height = renderer.cell_height;
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, width, height) ==
            BOXRIG_STATUS_OK);
        boxrig_ui_allocation_input allocation = {0};
        boxrig_ui_layout_result layout;
        allocation.allocation_width = 12 * width * BOXRIG_UI_FIXED_ONE;
        allocation.allocation_height = 2 * height * BOXRIG_UI_FIXED_ONE;
        allocation.cell_width = width * BOXRIG_UI_FIXED_ONE;
        allocation.cell_height = height * BOXRIG_UI_FIXED_ONE;
        allocation.cell_metrics_revision = 1;
        CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
        char text[512];
        int length = snprintf(text, sizeof(text), "\033[?25l\033[2J\033[H"
            "\033_Ga=T,f=32,s=1,v=1,c=2,r=1,z=-1,C=1;AP8A/w==\033\\"
            "\033[H\033[%u;4;9m\033]8;;https://example.com\007AB\033]8;;\007"
            "\033[2;1Hfi\xe7\x95\x8c \327\220\327\221\327\222\033[0m",
            reference == 0 ? 5u : 8u);
        CHECK(length > 0 && (size_t)length < sizeof(text));
        size_t consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, (size_t)length,
            &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
        size_t calls = 0;
        CHECK(boxrig_ui_view_set_color_filter(view, count_colors, &calls) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        boxrig_ui_hit_test_result first, last;
        CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
            BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_hyperlink_hover(view, &first) == BOXRIG_UI_OK);
        boxrig_text_search_options search = {0};
        search.pattern = (const uint8_t *)"AB";
        search.pattern_length = 2;
        CHECK(boxrig_ui_view_begin_search(view, &search) == BOXRIG_UI_OK);
        boxrig_text_search_progress progress;
        size_t used;
        do {
            CHECK(boxrig_ui_view_resume_search(view, 32, &used, &progress) == BOXRIG_UI_OK);
        } while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS);
        CHECK(progress == BOXRIG_TEXT_SEARCH_MATCH);
        CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
            ((selected_fill ? 0 : height) + 1) * BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER,
            &first) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_hit_test(view, (6 * width + 1) * BOXRIG_UI_FIXED_ONE,
            ((selected_fill ? 0 : height) + 1) * BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
        if (selected_fill) {
            boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
            style.presentation = BOXRIG_UI_SELECTION_FILL_OUTLINE;
            style.background = (boxrig_rgb){120, 180, 240};
            style.background_opacity = 128;
            style.marker_opacity = 160;
            style.foreground_policy = BOXRIG_UI_SELECTION_FOREGROUND_RGB;
            style.foreground = (boxrig_rgb){25, 90, 230};
            CHECK(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
        }
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *on = renderer.capture(renderer.context);
        CHECK(on != NULL);
        if (reference == 0) {
            size_t initial_calls = calls;
            renderer.reset_statistics(renderer.context);
            CHECK(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            hidden_blink = renderer.capture(renderer.context);
            CHECK(hidden_blink != NULL && memcmp(on, hidden_blink, count * sizeof(*on)) != 0);
            CHECK(hidden_blink[(height / 2) * BOXRIG_TEST_COLOR_WIDTH + width / 2] == (selected_fill ? 0x3cd978u : 0x00ff00u));
            CHECK(calls == initial_calls);
            boxrig_test_color_statistics statistics;
            CHECK(renderer.statistics(renderer.context, &statistics) == 0);
            CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
            CHECK(boxrig_ui_view_update_monotonic_time(view, 1000) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            uint32_t *again = renderer.capture(renderer.context);
            CHECK(again != NULL && memcmp(on, again, count * sizeof(*on)) == 0);
            free(again);
        } else {
            /* Concealed text supplies the same underlay/markers without glyphs. */
            CHECK(memcmp(hidden_blink, on, count * sizeof(*on)) == 0);
        }
        free(on);
        renderer.destroy(renderer.context);
        boxrig_ui_view_destroy(view);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
    }
    free(hidden_blink);
    return true;
}

bool boxrig_test_renderer_blink_composition(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    return renders_blink_composition(create, false);
}

bool boxrig_test_renderer_selection_blink(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    return renders_blink_composition(create, true);
}

bool boxrig_test_renderer_cursor_style(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    return boxrig_test_renderer_cursor_style_at_scale(create, 3, 3);
}

static boxrig_ui_color_filter_result filled_cursor_filter(
    const boxrig_ui_color_filter_input *input, void *context)
{
    (void)context;
    boxrig_ui_color_filter_result result = input->colors;
    if (input->target == BOXRIG_UI_COLOR_CELL) result.background = (boxrig_rgb){200, 210, 220};
    if (input->target == BOXRIG_UI_COLOR_CURSOR) result.marker = (boxrig_rgb){24, 32, 40};
    return result;
}

bool boxrig_test_renderer_filled_cursor(
    bool (*create)(boxrig_test_color_renderer *renderer), unsigned int stroke)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2; config.columns = 12;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    vc.blink.cursor_enabled = false;
    vc.blink.text_enabled = false;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_test_color_renderer renderer = {0};
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    const size_t count = BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT;
    const unsigned width = renderer.cell_width, height = renderer.cell_height;
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.block_presentation = BOXRIG_UI_CURSOR_BLOCK_FILL;
    style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
    style.color = (boxrig_rgb){240, 208, 128};
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    const char *texts[] = {"L R", "LMR", "L\347\225\214R", "Le\314\201R",
        "L\327\220\327\221R", "L\033[8mM\033[28mR", "L\347\225\214R",
        "L👩‍💻R", "          界"};
    const unsigned columns[] = {1, 1, 1, 1, 2, 1, 1, 1, 10};
    const unsigned spans[] = {1, 1, 2, 1, 1, 1, 2, 2, 2};
    const unsigned positions[] = {2, 2, 2, 2, 2, 2, 3, 2, 12};
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BLOCK;
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    for (unsigned sample = 0; sample < sizeof(texts)/sizeof(*texts); sample++) {
        char text[256];
        int length = snprintf(text, sizeof(text),
            "\033[0;48;2;16;32;48;38;2;224;224;224m\033[2J\033[H%s\033[1;%uH\033[2 q\033[?25l",
            texts[sample], positions[sample]);
        size_t consumed;
        CHECK(length > 0 && (size_t)length < sizeof(text));
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, (size_t)length,
            &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *underlay = renderer.capture(renderer.context);
        CHECK(underlay != NULL);
        renderer.reset_statistics(renderer.context);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25h", 6,
            &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *filled = renderer.capture(renderer.context);
        CHECK(filled != NULL);
        size_t fill_count = 0, ink_count = 0;
        size_t column_fill[2] = {0};
        unsigned darkest = 765;
        for (size_t at = 0; at < count; at++) {
            unsigned x = at % BOXRIG_TEST_COLOR_WIDTH, y = at / BOXRIG_TEST_COLOR_WIDTH;
            bool inside = y < height && x >= columns[sample]*width &&
                x < (columns[sample]+spans[sample])*width;
            if (!inside) CHECK(filled[at] == underlay[at]);
            else if (filled[at] == 0xf0d080u) {
                fill_count++;
                column_fill[x / width - columns[sample]]++;
            }
            else {
                ink_count++;
                unsigned sum = ((filled[at] >> 16)&255) + ((filled[at] >> 8)&255) + (filled[at]&255);
                if (sum < darkest) darkest = sum;
            }
        }
        /* Color emoji can cover nearly all of the cursor background. */
        unsigned minimum_fill = sample == 7 ? 0 : width*height/4;
        if (fill_count <= minimum_fill) fprintf(stderr,
            "cursor fill sample=%u fill=%zu ink=%zu area=%u\n", sample, fill_count, ink_count, width*height);
        CHECK(fill_count > minimum_fill);
        for (unsigned column = 0; column < spans[sample]; column++)
            CHECK(column_fill[column] > minimum_fill);
        if (sample != 0 && sample != 5) CHECK(ink_count != 0 && darkest < 400);
        else {
            if (fill_count != width*height) fprintf(stderr, "filled blank sample=%u fill=%zu area=%u\n",
                sample, fill_count, width*height);
            CHECK(fill_count == width*height);
        }
        if (spans[sample] == 2) {
            CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            uint32_t *outlined = renderer.capture(renderer.context);
            CHECK(outlined != NULL);
            CHECK(outlined[(height/2)*BOXRIG_TEST_COLOR_WIDTH + columns[sample]*width] == 0xf0d080u);
            CHECK(outlined[(height/2)*BOXRIG_TEST_COLOR_WIDTH + (columns[sample]+2)*width-1] == 0xf0d080u);
            free(outlined);
            CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
        }
        if (sample == 2) {
            /* Restore both old columns when moving onto a one-cell cursor. */
            const char move[] = "\033[2;6H";
            CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)move, sizeof(move)-1,
                &consumed) == BOXRIG_STATUS_OK);
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        }
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25l", 6,
            &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *hidden = renderer.capture(renderer.context);
        if (hidden != NULL) for (size_t at = 0; at < count; at++) {
            if (hidden[at] != underlay[at]) {
                fprintf(stderr, "cursor restore sample=%u x=%zu y=%zu old=%06x new=%06x\n",
                    sample, at%BOXRIG_TEST_COLOR_WIDTH, at/BOXRIG_TEST_COLOR_WIDTH,
                    underlay[at], hidden[at]);
                break;
            }
        }
        CHECK(hidden != NULL && memcmp(hidden, underlay, count*sizeof(*hidden)) == 0);
        boxrig_test_color_statistics stats;
        CHECK(renderer.statistics(renderer.context, &stats) == 0);
        CHECK(stats.row_shape_count == 0 && stats.glyph_upload_count == 0);
        free(hidden); free(filled); free(underlay);
    }
    /* DECSCUSR still selects each shape; Ps=0 and omission normalize to 1.
     * Count pixels on a blank cell, where glyph antialiasing is irrelevant. */
    style.shape = BOXRIG_UI_CURSOR_SHAPE_TERMINAL;
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_DEFAULT;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    for (unsigned ps = 0; ps <= 7; ps++) {
        char text[64];
        int length = snprintf(text, sizeof(text), "\033[2;5H\033[?25h\033[%s q",
            (const char *const[]){"0", "1", "2", "3", "4", "5", "6", ""}[ps]);
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, (size_t)length,
            &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *pixels = renderer.capture(renderer.context);
        CHECK(pixels != NULL);
        size_t colored = 0;
        for (unsigned y = height; y < 2*height; y++)
            for (unsigned x = 4*width; x < 5*width; x++)
                if (pixels[y*BOXRIG_TEST_COLOR_WIDTH+x] == 0xf0d080u) colored++;
        CHECK(colored == (ps <= 2 || ps == 7 ? width*height : ps <= 4 ? width*stroke : height*stroke));
        free(pixels);
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.blinking == (ps == 0 || ps == 1 || ps == 3 || ps == 5 || ps == 7));
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033P$q q\033\\", 8,
            &consumed) == BOXRIG_STATUS_OK);
        char expected[16];
        int n = snprintf(expected, sizeof(expected), "\033P1$r%u q\033\\", ps == 0 || ps == 7 ? 1 : ps);
        const uint8_t *reply;
        CHECK(boxrig_terminal_output_peek(terminal, &reply) == (size_t)n);
        CHECK(memcmp(reply, expected, (size_t)n) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, (size_t)n) == BOXRIG_STATUS_OK);
    }
    /* Similar colors choose contrasting ink, including terminal inversion
     * and a filter that independently changes cell and cursor colors. */
    for (unsigned variant = 0; variant < 3; variant++) {
        style.color = variant == 1 ? (boxrig_rgb){224,224,224} : (boxrig_rgb){16,32,48};
        CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_set_color_filter(view,
            variant == 2 ? filled_cursor_filter : NULL, NULL) == BOXRIG_UI_OK);
        const char *text = variant == 1
            ? "\033[0;48;2;16;32;48;38;2;224;224;224;7m\033[2J\033[H M \033[1;2H\033[2 q"
            : "\033[0;48;2;16;32;48;38;2;224;224;224m\033[2J\033[H M \033[1;2H\033[2 q";
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
            &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        uint32_t *pixels = renderer.capture(renderer.context);
        CHECK(pixels != NULL);
        const uint32_t fill = variant == 1 ? 0xe0e0e0u : variant == 2 ? 0x182028u : 0x102030u;
        unsigned minimum = 765, maximum = 0, filled = 0;
        for (unsigned y = 0; y < height; y++) for (unsigned x = width; x < 2*width; x++) {
            uint32_t pixel = pixels[y*BOXRIG_TEST_COLOR_WIDTH+x];
            unsigned sum = ((pixel>>16)&255) + ((pixel>>8)&255) + (pixel&255);
            if (sum < minimum) minimum = sum;
            if (sum > maximum) maximum = sum;
            if (pixel == fill) filled++;
        }
        CHECK(filled != 0);
        CHECK(variant == 1 ? minimum < 200 : maximum > 400);
        free(pixels);
    }
    CHECK(boxrig_ui_view_set_color_filter(view, NULL, NULL) == BOXRIG_UI_OK);
    const char *blank = "\033[0;48;2;16;32;48m\033[2J\033[2;5H\033[1 q";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)blank, strlen(blank),
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    style.color = (boxrig_rgb){240,208,128};
    style.shape = BOXRIG_UI_CURSOR_SHAPE_OUTLINE;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *outline = renderer.capture(renderer.context);
    CHECK(outline != NULL);
    CHECK(outline[(height+height/2)*BOXRIG_TEST_COLOR_WIDTH+4*width+width/2] == 0x102030u);
    free(outline);
    style.shape = BOXRIG_UI_CURSOR_SHAPE_TERMINAL;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    vc.blink.cursor_enabled = true;
    CHECK(boxrig_ui_view_set_blink_config(view, &vc.blink) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *on = renderer.capture(renderer.context);
    CHECK(on != NULL);
    renderer.reset_statistics(renderer.context);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *off = renderer.capture(renderer.context);
    CHECK(off != NULL);
    size_t differences = 0;
    for (size_t at = 0; at < count; at++) if (on[at] != off[at]) {
        CHECK(at/BOXRIG_TEST_COLOR_WIDTH >= height && at/BOXRIG_TEST_COLOR_WIDTH < 2*height);
        CHECK(at%BOXRIG_TEST_COLOR_WIDTH >= 4*width && at%BOXRIG_TEST_COLOR_WIDTH < 5*width);
        differences++;
    }
    CHECK(differences == width*height);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 1000) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *again = renderer.capture(renderer.context);
    CHECK(again != NULL && memcmp(on, again, count*sizeof(*on)) == 0);
    boxrig_test_color_statistics stats;
    CHECK(renderer.statistics(renderer.context, &stats) == 0);
    CHECK(stats.row_shape_count == 0 && stats.glyph_upload_count == 0);
    free(again); free(off); free(on);

    /* The cursor covers the image and selection but restores both on hide.
     * IME preedit suppresses the terminal cursor until it is cancelled. */
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BLOCK;
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED;
    CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    vc.blink.cursor_enabled = false;
    vc.blink.text_enabled = true;
    CHECK(boxrig_ui_view_set_blink_config(view, &vc.blink) == BOXRIG_UI_OK);
    const char blink_text[] = "\033[0;5m\033[2J\033[HLMR\033[1;2H\033[2 q";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)blink_text, sizeof(blink_text)-1,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *text_on = renderer.capture(renderer.context);
    CHECK(text_on != NULL);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 1500) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *text_off = renderer.capture(renderer.context);
    CHECK(text_off != NULL);
    for (unsigned y = 0; y < height; y++) for (unsigned x = width; x < 2*width; x++)
        CHECK(text_off[y*BOXRIG_TEST_COLOR_WIDTH+x] == 0xf0d080u);
    CHECK(memcmp(text_on, text_off, count*sizeof(*text_on)) != 0);
    CHECK(boxrig_ui_view_update_monotonic_time(view, 2000) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *text_again = renderer.capture(renderer.context);
    CHECK(text_again != NULL && memcmp(text_on, text_again, count*sizeof(*text_on)) == 0);
    free(text_on); free(text_off); free(text_again);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, width, height) == BOXRIG_STATUS_OK);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 12*width*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 2*height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = width*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = height*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const char layered[] = "\033[0;48;2;16;32;48m\033[2J\033[HLMR\033[1;2H\033[2 q\033[?25l"
        "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,z=0,C=1;AP8A/w==\033\\";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)layered, sizeof(layered)-1,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    boxrig_ui_hit_test_result first, last;
    CHECK(boxrig_ui_view_hit_test(view, (width+1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (2*width-1)*BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *layer_underlay = renderer.capture(renderer.context);
    CHECK(layer_underlay != NULL);
    CHECK(layer_underlay[(height/2)*BOXRIG_TEST_COLOR_WIDTH+width+width/2] == 0x00ff00u);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25h", 6, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *layer_cursor = renderer.capture(renderer.context);
    CHECK(layer_cursor != NULL);
    size_t fill_pixels = 0, ink_pixels = 0;
    for (size_t at = 0; at < count; at++) {
        unsigned x = at%BOXRIG_TEST_COLOR_WIDTH, y = at/BOXRIG_TEST_COLOR_WIDTH;
        if (y < height && x >= width && x < 2*width) {
            if (layer_cursor[at] == 0xf0d080u) fill_pixels++;
            else ink_pixels++;
        } else CHECK(layer_cursor[at] == layer_underlay[at]);
    }
    CHECK(fill_pixels > width*height/4 && ink_pixels > 0);
    boxrig_composition *composition = NULL;
    CHECK(boxrig_composition_create(terminal, boxrig_composition_limits_default(),
        &composition) == BOXRIG_COMPOSITION_OK);
    boxrig_composition_event event = {0};
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = " "; event.text_length = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    uint32_t *preedit = renderer.capture(renderer.context);
    CHECK(preedit != NULL);
    for (unsigned y = 0; y < height; y++) for (unsigned x = width; x < 2*width; x++)
        CHECK(preedit[y*BOXRIG_TEST_COLOR_WIDTH+x] != 0xf0d080u);
    free(preedit);
    event = (boxrig_composition_event){0};
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    uint32_t *cancelled = renderer.capture(renderer.context);
    CHECK(cancelled != NULL && memcmp(cancelled, layer_cursor, count*sizeof(*cancelled)) == 0);
    free(cancelled);
    boxrig_composition_destroy(composition);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25l", 6, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *restored = renderer.capture(renderer.context);
    CHECK(restored != NULL && memcmp(restored, layer_underlay, count*sizeof(*restored)) == 0);
    free(restored); free(layer_cursor); free(layer_underlay);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS filled cursor contrast, clipping, concealment, DECSCUSR, query, layers and IME");
    return true;
}
