/* boxrig - Under MIT. */
#include "color_renderer_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef struct filter_state {
    bool identity;
    bool independent_underline;
    uint8_t adjustment;
    size_t calls;
} filter_state;

static boxrig_ui_color_filter_result correct_colors(
    const boxrig_ui_color_filter_input *input, void *opaque)
{
    filter_state *state = opaque;
    state->calls++;
    boxrig_ui_color_filter_result result =
        boxrig_ui_color_filter_result_default(input);
    if (!state->identity) {
        result.foreground = (boxrig_rgb){255, 255, 0};
        result.background = input->target == BOXRIG_UI_COLOR_SURFACE
            ? (boxrig_rgb){0, 0, state->adjustment}
            : (boxrig_rgb){input->colors.foreground.red,
                input->colors.background.green, state->adjustment};
        if (state->independent_underline) {
            result.underline = (boxrig_rgb){0, 255, 255};
            result.underline_follows_foreground = false;
        }
    }
    return result;
}

static uint32_t get_pixel(const uint32_t *image, int x, int y)
{
    return image[y * BOXRIG_TEST_COLOR_WIDTH + x];
}

static bool pixel_is(uint32_t *image, int x, int y, unsigned long rgb)
{
    return (get_pixel(image, x, y) & 0xffffffu) == rgb;
}

static bool renders_filtered_cells_and_surface(bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *other = NULL;
    boxrig_test_color_renderer renderer = {0};
    config.rows = 2;
    config.columns = 12;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &other) == BOXRIG_UI_OK);
    unsigned int width = renderer.cell_width;
    unsigned int height = renderer.cell_height;
    const char text[] = "\033[?25l\033[38;2;40;50;60;48;2;70;80;90;4mA "
        "\033[7mB\033[0;8;4;9mC\033[0mD"
        "\033[8;4;9m\360\237\230\200\033[0m";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *original = renderer.capture(renderer.context);
    CHECK(original != NULL);

    filter_state state = {true, false, 23, 0};
    CHECK(boxrig_ui_view_set_color_filter(view, correct_colors, &state) == BOXRIG_UI_OK);
    renderer.reset_statistics(renderer.context);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *identity = renderer.capture(renderer.context);
    CHECK(identity != NULL);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 600; x++) {
            CHECK(get_pixel(original, x, y) == get_pixel(identity, x, y));
        }
    }
    free(identity);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0 && statistics.row_raster_count == 2);

    state.identity = false;
    CHECK(boxrig_ui_view_invalidate_colors(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *filtered = renderer.capture(renderer.context);
    CHECK(filtered != NULL);
    CHECK(pixel_is(filtered, (int)width + 1, 0, 0x285017));
    CHECK(pixel_is(filtered, (int)(2 * width) + 1, 0, 0x463217));
    CHECK(pixel_is(filtered, 599, 199, 0x000017));
    CHECK(pixel_is(filtered, (int)width + 1, (int)height - 2, 0xffff00));
    /* Concealed text, underline and strike must not reappear after filtering. */
    unsigned long concealed_background = get_pixel(filtered, (int)(3 * width), 0);
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 3 * width; x < 4 * width; x++) {
            CHECK(get_pixel(filtered, (int)x, (int)y) == concealed_background);
        }
        /* Concealment also excludes the two cells of a color-glyph cluster. */
        for (unsigned int x = 5 * width; x < 7 * width; x++) {
            CHECK(get_pixel(filtered, (int)x, (int)y) == concealed_background);
        }
    }
    free(filtered);
    size_t calls = state.calls;
    renderer.reset_statistics(renderer.context);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_raster_count == 0 && statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
    CHECK(calls == state.calls);
    state.independent_underline = true;
    state.adjustment = 31;
    CHECK(boxrig_ui_view_set_color_filter(view, correct_colors, &state) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    filtered = renderer.capture(renderer.context);
    CHECK(filtered != NULL);
    CHECK(pixel_is(filtered, (int)width + 1, (int)height - 2, 0x00ffff));
    CHECK(pixel_is(filtered, 599, 199, 0x00001f));
    free(filtered);

    CHECK(renderer.draw(renderer.context, other, NULL) == 0);
    identity = renderer.capture(renderer.context);
    CHECK(identity != NULL);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 600; x++) {
            CHECK(get_pixel(original, x, y) == get_pixel(identity, x, y));
        }
    }
    free(identity);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(boxrig_ui_view_set_color_filter(view, NULL, NULL) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    identity = renderer.capture(renderer.context);
    CHECK(identity != NULL);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 600; x++) {
            CHECK(get_pixel(original, x, y) == get_pixel(identity, x, y));
        }
    }
    free(identity);
    free(original);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

typedef struct state_filter_context {
    bool identity;
    uint32_t targets;
    uint32_t states;
    bool marker_without_underlay;
    boxrig_ui_color_filter_input last[BOXRIG_UI_COLOR_IME_MARKER + 1];
} state_filter_context;

static boxrig_ui_color_filter_result state_colors(
    const boxrig_ui_color_filter_input *input, void *opaque)
{
    state_filter_context *context = opaque;
    context->targets |= 1u << input->target;
    context->states |= input->state_flags;
    context->last[input->target] = *input;
    boxrig_ui_color_filter_result result =
        boxrig_ui_color_filter_result_default(input);
    if (input->target >= BOXRIG_UI_COLOR_CURSOR &&
        input->target != BOXRIG_UI_COLOR_IME && !input->underlay_valid) {
        context->marker_without_underlay = true;
    }
    if (context->identity) return result;
    if (input->target == BOXRIG_UI_COLOR_CELL) {
        result.background.green = (uint8_t)(16 * (input->state_flags & 7u));
        result.background.blue =
            (input->state_flags & BOXRIG_UI_COLOR_FOCUSED) != 0 ? 40 : 10;
        if ((input->state_flags & BOXRIG_UI_COLOR_CURSOR_CELL) != 0) {
            result.background.blue += 5;
        }
    } else if (input->target == BOXRIG_UI_COLOR_SURFACE) {
        result.background.blue =
            (input->state_flags & BOXRIG_UI_COLOR_FOCUSED) != 0 ? 40 : 10;
    } else if (input->target == BOXRIG_UI_COLOR_IME) {
        result.foreground = (boxrig_rgb){250, 240, 20};
        result.background = (boxrig_rgb){25, 45, 65};
        result.underline = (boxrig_rgb){255, 0, 255};
        result.underline_follows_foreground = false;
    } else if (input->target == BOXRIG_UI_COLOR_CURSOR) {
        result.marker = (boxrig_rgb){90, 150, (uint8_t)(100 + input->cursor_shape)};
    } else if (input->target == BOXRIG_UI_COLOR_IME_MARKER) {
        result.marker = (boxrig_rgb){12, 200, 60};
    } else {
        result.marker = (boxrig_rgb){input->underlay.background.red, 240,
            input->underlay.background.green};
    }
    return result;
}

static bool renders_state_colors_and_markers(bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_test_color_renderer renderer = {0};
    config.rows = 2;
    config.columns = 12;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    unsigned int width = renderer.cell_width;
    unsigned int height = renderer.cell_height;
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result layout;
    allocation.allocation_width = 12 * width * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 2 * height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = width * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const char text[] = "\033[?25l\033[48;2;31;0;0m \033[48;2;61;0;0m "
        "\033[0m\033[2;1H\033]8;;https://example.com\007AB\033]8;;\007"
        " \xe7\x95\x8c \327\220\327\221";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    state_filter_context context = {0};
    CHECK(boxrig_ui_view_set_color_filter(view, state_colors, &context) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    boxrig_ui_hit_test_result first, last;
    CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER,
        &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (2 * width - 1) * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    renderer.reset_statistics(renderer.context);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(pixel_is(pixels, 2, 2, 0x1f100a));
    CHECK(pixel_is(pixels, (int)width + 2, 2, 0x3d100a));
    CHECK(pixel_is(pixels, 2, 0, 0x1ff010));
    CHECK(pixel_is(pixels, (int)width + 2, 0, 0x3df010));
    /* A color boundary inside an outline must not gain a vertical edge. */
    CHECK(pixel_is(pixels, (int)width - 1, (int)height / 2, 0x1f100a));
    CHECK(pixel_is(pixels, (int)width, (int)height / 2, 0x3d100a));
    free(pixels);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0 && statistics.row_raster_count == 1);
    CHECK(!context.marker_without_underlay);
    CHECK(context.last[BOXRIG_UI_COLOR_SELECTION_MARKER].marker_source.role == BOXRIG_UI_COLOR_UI);

    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL && pixel_is(pixels, 2, 0, 0x1f000a));
    free(pixels);

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
        (height + 1) * BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_hyperlink_hover(view, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER,
        &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, (2 * width - 1) * BOXRIG_UI_FIXED_ONE,
        (height + 1) * BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(pixel_is(pixels, 2, (int)height + 1, 0x10700a));
    CHECK(pixel_is(pixels, 2, (int)(2 * height) - 1, 0x10f070));
    free(pixels);
    CHECK((context.states & 7u) == 7u);
    CHECK(boxrig_ui_view_set_color_filter(view, NULL, NULL) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *unfiltered = renderer.capture(renderer.context);
    CHECK(unfiltered != NULL);
    context.identity = true;
    CHECK(boxrig_ui_view_set_color_filter(view, state_colors, &context) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 600; x++) {
            CHECK(get_pixel(unfiltered, x, y) == get_pixel(pixels, x, y));
        }
    }
    free(pixels);
    free(unfiltered);
    context.identity = false;
    CHECK(boxrig_ui_view_invalidate_colors(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK((context.targets & ((1u << BOXRIG_UI_COLOR_SEARCH_MARKER) |
        (1u << BOXRIG_UI_COLOR_HOVER_MARKER))) ==
        ((1u << BOXRIG_UI_COLOR_SEARCH_MARKER) | (1u << BOXRIG_UI_COLOR_HOVER_MARKER)));

    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_search(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_hyperlink_hover(view) == BOXRIG_UI_OK);
    const char cursor[] = "\033[?25h\033[1;1H\033[2 q";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)cursor,
        sizeof(cursor) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(pixel_is(pixels, 2, 2, 0x1f000f));
    CHECK(pixel_is(pixels, 2, 0, 0x5a9664));
    free(pixels);
    CHECK(context.last[BOXRIG_UI_COLOR_CURSOR].marker_source.role == BOXRIG_UI_COLOR_DEFAULT_CURSOR);
    CHECK(context.last[BOXRIG_UI_COLOR_CURSOR].underlay.background.blue == 15);

    const char moved[] = "\033[1;2H\033[6 q";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)moved,
        sizeof(moved) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    renderer.reset_statistics(renderer.context);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(pixel_is(pixels, 2, 0, 0x1f000a));
    CHECK(pixel_is(pixels, (int)width + 1, 2, 0x5a9666));
    free(pixels);
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);

    const char underline_cursor[] = "\033[4 q";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)underline_cursor,
        sizeof(underline_cursor) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(pixel_is(pixels, (int)width + 2, (int)height - 2, 0x5a9665));
    CHECK(pixel_is(pixels, (int)width + 1, 2, 0x3d000f));
    free(pixels);

    const size_t word_columns[] = {3, 6};
    for (size_t index = 0; index < 2; index++) {
        size_t column = word_columns[index];
        CHECK(boxrig_ui_view_hit_test(view, (column * width + 1) * BOXRIG_UI_FIXED_ONE,
            (height + 1) * BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_WORD,
            &first) == BOXRIG_UI_OK);
        renderer.reset_statistics(renderer.context);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        pixels = renderer.capture(renderer.context);
        CHECK(pixels != NULL);
        CHECK(pixel_is(pixels, (int)(column * width + 2), (int)height, 0x10f010));
        CHECK(pixel_is(pixels, (int)((column + 1) * width + 2), (int)height, 0x10f010));
        free(pixels);
        CHECK(renderer.statistics(renderer.context, &statistics) == 0);
        CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
        CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    }

    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_focused(view));
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL && pixel_is(pixels, 599, 199, 0x101028));
    CHECK(pixel_is(pixels, 2, 2, 0x1f0028));
    free(pixels);
    CHECK((context.states & BOXRIG_UI_COLOR_FOCUSED) != 0);
    CHECK(!context.marker_without_underlay);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool renders_ime_colors(bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_test_color_renderer renderer = {0};
    boxrig_composition *composition = NULL;
    config.rows = 2;
    config.columns = 12;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_composition_create(terminal, boxrig_composition_limits_default(),
        &composition) == BOXRIG_COMPOSITION_OK);
    unsigned int width = renderer.cell_width;
    unsigned int height = renderer.cell_height;
    const char text[] = "\033[1;2H";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_composition_segment segment = {0, 3, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE};
    boxrig_composition_event event = {0};
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = "ab ";
    event.text_length = 3;
    event.selection_length = 1;
    event.segments = &segment;
    event.segment_count = 1;
    event.caret_offset = 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    uint32_t *original = renderer.capture(renderer.context);
    CHECK(original != NULL);
    state_filter_context context = {0};
    context.identity = true;
    CHECK(boxrig_ui_view_set_color_filter(view, state_colors, &context) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    uint32_t *pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 600; x++) {
            CHECK(get_pixel(original, x, y) == get_pixel(pixels, x, y));
        }
    }
    free(pixels);
    free(original);
    CHECK((context.targets & (1u << BOXRIG_UI_COLOR_IME)) != 0);
    CHECK((context.targets & (1u << BOXRIG_UI_COLOR_IME_MARKER)) != 0);
    CHECK((context.targets & (1u << BOXRIG_UI_COLOR_CURSOR)) == 0);
    CHECK((context.states & BOXRIG_UI_COLOR_CURSOR_CELL) == 0);
    context.identity = false;
    CHECK(boxrig_ui_view_invalidate_colors(view) == BOXRIG_UI_OK);
    renderer.reset_statistics(renderer.context);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL);
    CHECK(pixel_is(pixels, (int)(3 * width) + 3, 2, 0x192d41));
    CHECK(pixel_is(pixels, (int)(3 * width) + 3, (int)height - 2, 0xff00ff));
    boxrig_composition_view preedit;
    CHECK(boxrig_composition_get_view(composition, &preedit) == BOXRIG_COMPOSITION_OK);
    CHECK(pixel_is(pixels, (int)preedit.candidate.spot_x,
        (int)height / 2, 0x0cc83c));
    free(pixels);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay.background.red == 25);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay.background.green == 45);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay.background.blue == 65);
    CHECK(!context.marker_without_underlay);
    boxrig_test_color_statistics statistics;
    CHECK(renderer.statistics(renderer.context, &statistics) == 0);
    CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
    /* A caret after the preedit still has the terminal cell as its underlay. */
    event.caret_offset = 3;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay_valid);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay.background.red == 16);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay.background.green == 0);
    CHECK(context.last[BOXRIG_UI_COLOR_IME_MARKER].underlay.background.blue == 10);
    event = (boxrig_composition_event){0};
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(renderer.draw(renderer.context, view, composition) == 0);
    pixels = renderer.capture(renderer.context);
    CHECK(pixels != NULL && pixel_is(pixels, (int)(2 * width),
        (int)height / 2, 0x10000a));
    CHECK((context.states & BOXRIG_UI_COLOR_CURSOR_CELL) != 0);
    free(pixels);
    boxrig_composition_destroy(composition);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool retains_colors_after_cache_eviction(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_test_color_renderer renderer = {0};
    filter_state state = {true, false, 0, 0};
    config.rows = 12;
    config.columns = 60;
    CHECK(create(&renderer));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result layout;
    allocation.allocation_width = config.columns * renderer.cell_width * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = config.rows * renderer.cell_height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = renderer.cell_width * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = renderer.cell_height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_color_filter(view, correct_colors, &state) == BOXRIG_UI_OK);
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25l", 6,
        &consumed) == BOXRIG_STATUS_OK);
    for (size_t row = 0; row < config.rows; row++) {
        for (size_t column = 0; column < config.columns; column++) {
            size_t index = row * config.columns + column;
            char text[100];
            int length = snprintf(text, sizeof(text),
                "\033[%zu;%zuH\033[48;2;%zu;%zu;73m ",
                row + 1, column + 1, index % 256, index / 256);
            CHECK(length > 0 && (size_t)length < sizeof(text));
            CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
                (size_t)length, &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == (size_t)length);
        }
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    uint32_t *original = renderer.capture(renderer.context);
    CHECK(original != NULL);
    for (size_t row = 0; row < config.rows; row++) {
        for (size_t column = 0; column < config.columns; column++) {
            size_t index = row * config.columns + column;
            int x = (int)(column * renderer.cell_width + 1u);
            int y = (int)(row * renderer.cell_height + 1u);
            if (x < BOXRIG_TEST_COLOR_WIDTH && y < BOXRIG_TEST_COLOR_HEIGHT) {
                CHECK(pixel_is(original, x, y,
                    ((index % 256) << 16) | ((index / 256) << 8) | 73u));
            }
        }
    }
    for (size_t phase = 0; phase < 2; phase++) {
        if (phase != 0) {
            boxrig_ui_hit_test_result first, last;
            CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
                BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER,
                &first) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_hit_test(view,
                (config.columns * renderer.cell_width - 1) * BOXRIG_UI_FIXED_ONE,
                (config.rows * renderer.cell_height - 1) * BOXRIG_UI_FIXED_ONE,
                &last) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_update_selection(view, &last) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, view, NULL) == 0);
            free(original);
            original = renderer.capture(renderer.context);
            CHECK(original != NULL);
        }
        /* Exercise another consumer of the bounded result cache. Visible paints
         * must survive eviction without depending on that cache's capacity. */
        boxrig_cell_view cell;
        boxrig_ui_color_filter_input input;
        boxrig_ui_color_filter_result result;
        CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 255,
            &result, &input) == BOXRIG_UI_OK);
        for (size_t index = 0; index < 4096; index++) {
            input.colors.background = (boxrig_rgb){(uint8_t)index,
                (uint8_t)(index >> 8), 241};
            CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
        }
        size_t calls = state.calls;
        renderer.reset_statistics(renderer.context);
        boxrig_ui_dirty dirty = {0, config.rows, 0, config.columns};
        CHECK(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
        CHECK(renderer.draw(renderer.context, view, NULL) == 0);
        CHECK(state.calls == calls);
        boxrig_test_color_statistics statistics;
        CHECK(renderer.statistics(renderer.context, &statistics) == 0);
        CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
        uint32_t *repeated = renderer.capture(renderer.context);
        CHECK(repeated != NULL && memcmp(original, repeated,
            BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT * sizeof(*original)) == 0);
        free(repeated);
    }
    free(original);
    size_t calls = state.calls;
    CHECK(boxrig_ui_view_invalidate_colors(view) == BOXRIG_UI_OK);
    CHECK(renderer.draw(renderer.context, view, NULL) == 0);
    CHECK(state.calls > calls);
    renderer.destroy(renderer.context);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

bool boxrig_test_renderer_colors(
    bool (*create)(boxrig_test_color_renderer *renderer))
{
    return renders_filtered_cells_and_surface(create) &&
        renders_state_colors_and_markers(create) && renders_ime_colors(create) &&
        retains_colors_after_cache_eviction(create);
}
