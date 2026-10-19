/* boxrig - Under MIT. */
#include "selection_renderer_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static uint32_t pack_rgb(boxrig_rgb color)
{
    return ((uint32_t)color.red << 16) | ((uint32_t)color.green << 8) | color.blue;
}

static uint32_t blend_rgb(uint32_t base, uint32_t top, unsigned alpha)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8)
        result |= ((((base >> shift) & 255) * (255-alpha) +
            ((top >> shift) & 255) * alpha + 127) / 255) << shift;
    return result;
}

static bool equal_rgb(uint32_t actual, uint32_t expected)
{
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        int delta = (int)((actual >> shift) & 255) - (int)((expected >> shift) & 255);
        if (delta < -2 || delta > 2) return false;
    }
    return true;
}

static bool same_snapshot(const boxrig_text_snapshot *left,
    const boxrig_text_snapshot *right)
{
    size_t left_count, right_count;
    const uint8_t *left_text = boxrig_text_snapshot_data(left, &left_count);
    const uint8_t *right_text = boxrig_text_snapshot_data(right, &right_count);
    return left_count > 0 && left_count == right_count &&
        memcmp(left_text, right_text, left_count) == 0;
}

static bool check_pixels(boxrig_test_color_renderer *renderer, boxrig_ui_view *view,
    unsigned first_column, const boxrig_ui_selection_style *selection,
    const boxrig_ui_cursor_style *cursor)
{
    CHECK(renderer->draw(renderer->context, view, NULL) == 0);
    uint32_t *pixels = renderer->capture(renderer->context);
    CHECK(pixels != NULL);
    unsigned width = renderer->cell_width, height = renderer->cell_height;
    unsigned marker_width = (unsigned)selection->line_width_dip;
    unsigned cursor_width = (unsigned)cursor->line_width_dip;
    for (unsigned y = 0; y < 2*height; y++) for (unsigned x = 0; x < 12*width; x++) {
        uint32_t expected = 0x102030u;
        bool selected = y < height && x >= first_column*width &&
            x < (first_column+6)*width;
        if (selected && selection->presentation != BOXRIG_UI_SELECTION_OUTLINE)
            expected = blend_rgb(expected, pack_rgb(selection->background),
                selection->background_opacity);
        bool perimeter = y < marker_width || y >= height-marker_width ||
            x < first_column*width+marker_width ||
            x >= (first_column+6)*width-marker_width;
        if (selected && perimeter && selection->presentation != BOXRIG_UI_SELECTION_FILL)
            expected = blend_rgb(expected, pack_rgb(selection->marker),
                selection->marker_opacity);
        bool in_cursor = y >= height && x >= 9*width && x < 10*width;
        /* The established bar/underline presentation leaves one inner pixel
         * between the line and the corresponding cell edge. */
        if (in_cursor && ((cursor->shape == BOXRIG_UI_CURSOR_SHAPE_BAR &&
                x >= 9*width+1 && x < 9*width+1+cursor_width) ||
            (cursor->shape == BOXRIG_UI_CURSOR_SHAPE_UNDERLINE &&
                y >= 2*height-cursor_width-1 && y < 2*height-1)))
            expected = pack_rgb(cursor->color);
        uint32_t actual = pixels[y*BOXRIG_TEST_COLOR_WIDTH+x];
        if (!equal_rgb(actual, expected)) {
            fprintf(stderr, "view=%u x=%u y=%u actual=%06x expected=%06x\n",
                first_column, x, y, actual, expected);
            unsigned count = 0, min_x = BOXRIG_TEST_COLOR_WIDTH,
                min_y = BOXRIG_TEST_COLOR_HEIGHT, max_x = 0, max_y = 0;
            for (unsigned py = 0; py < BOXRIG_TEST_COLOR_HEIGHT; py++)
                for (unsigned px = 0; px < BOXRIG_TEST_COLOR_WIDTH; px++)
                    if (pixels[py*BOXRIG_TEST_COLOR_WIDTH+px] == pack_rgb(cursor->color)) {
                        count++;
                        if (px < min_x) min_x = px;
                        if (py < min_y) min_y = py;
                        if (px > max_x) max_x = px;
                        if (py > max_y) max_y = py;
                    }
            fprintf(stderr, "cursor color count=%u bounds=%u,%u-%u,%u\n",
                count, min_x, min_y, max_x, max_y);
            free(pixels);
            CHECK(false);
        }
    }
    free(pixels);
    return true;
}

static bool check_cursor_boxes(boxrig_test_color_renderer *renderer,
    boxrig_ui_view *view)
{
    boxrig_ui_cursor_style saved, style = boxrig_ui_cursor_style_default();
    CHECK(boxrig_ui_view_get_cursor_style(view, &saved) == BOXRIG_UI_OK);
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED;
    style.blink = BOXRIG_UI_CURSOR_BLINK_STEADY;
    style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
    style.color = (boxrig_rgb){255, 215, 0};
    const boxrig_ui_cursor_shape_policy shapes[] = {BOXRIG_UI_CURSOR_SHAPE_BLOCK,
        BOXRIG_UI_CURSOR_SHAPE_BAR, BOXRIG_UI_CURSOR_SHAPE_UNDERLINE};
    unsigned cw = renderer->cell_width, ch = renderer->cell_height;
    for (unsigned shape = 0; shape < 3; shape++) {
        style.shape = shapes[shape];
        CHECK(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
        /* Repeat focus transitions with no text updates or color filter. */
        for (unsigned change = 0; change < 4; change++) {
            bool focused = change % 2 == 0;
            CHECK(boxrig_ui_view_set_focused(view, focused) == BOXRIG_UI_OK);
            CHECK(renderer->draw(renderer->context, view, NULL) == 0);
            uint32_t *pixels = renderer->capture(renderer->context);
            CHECK(pixels != NULL);
            unsigned width = shape == 1 ? 4 : cw;
            unsigned height = shape == 2 ? 4 : ch;
            unsigned top = ch - height;
            for (unsigned y = 0; y < ch; y++) for (unsigned x = 0; x < cw; x++) {
                bool inside = x < width && y >= top;
                bool border = x == 0 || x == width-1 || y == top || y == ch-1;
                uint32_t expected = inside && (focused || border) ? 0xffd700 : 0x102030;
                uint32_t actual = pixels[(ch+y)*BOXRIG_TEST_COLOR_WIDTH+9*cw+x];
                if (!equal_rgb(actual, expected)) {
                    fprintf(stderr, "box shape=%u focus=%d x=%u y=%u got=%06x expected=%06x\n",
                        shape, focused, x, y, actual, expected);
                    free(pixels);
                    CHECK(false);
                }
            }
            free(pixels);
        }
    }
    CHECK(boxrig_ui_view_set_cursor_style(view, &saved) == BOXRIG_UI_OK);
    return true;
}

bool boxrig_test_renderer_decoration_views(
    const boxrig_test_decoration_factory *factories,
    const char *const *names, size_t factory_count)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 12;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    view_config.blink.cursor_enabled = false;
    view_config.blink.text_enabled = false;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *views[2] = {NULL, NULL};
    boxrig_text_snapshot *copies[2] = {NULL, NULL};
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    boxrig_ui_selection_style selections[2];
    boxrig_ui_cursor_style cursors[2];
    for (unsigned i = 0; i < 2; i++) {
        CHECK(boxrig_ui_view_create(model, &view_config, &views[i]) == BOXRIG_UI_OK);
        selections[i] = boxrig_ui_selection_style_default();
        selections[i].presentation = i == 0 ? BOXRIG_UI_SELECTION_FILL :
            BOXRIG_UI_SELECTION_OUTLINE;
        selections[i].background = (boxrig_rgb){20, 180, 220};
        selections[i].background_opacity = 128;
        selections[i].marker = (boxrig_rgb){220, 90, 30};
        selections[i].line_width_dip = 3;
        cursors[i] = boxrig_ui_cursor_style_default();
        cursors[i].shape = i == 0 ? BOXRIG_UI_CURSOR_SHAPE_BAR :
            BOXRIG_UI_CURSOR_SHAPE_UNDERLINE;
        cursors[i].color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
        cursors[i].color = i == 0 ? (boxrig_rgb){255, 80, 180} :
            (boxrig_rgb){255, 215, 0};
        cursors[i].line_width_dip = i == 0 ? 2 : 3;
        cursors[i].blink = BOXRIG_UI_CURSOR_BLINK_STEADY;
        CHECK(boxrig_ui_view_set_selection_style(views[i], &selections[i]) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_set_cursor_style(views[i], &cursors[i]) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_ui_view_acquire_layout_authority(views[0]) == BOXRIG_UI_OK);
    const char text[] = "\033[48;2;16;32;48m\033[2J\033[1;2H\033[8m"
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\x8b\xe3\x81\xaa"
        "\033[28m\033[4;1H"
        "X\xe7\x95\x8c" "e\xcc\x88 fi \327\220\327\221\327\222"
        "\033[2;10H\033[?25h\033[2 q";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text)-1, &consumed) == BOXRIG_STATUS_OK && consumed == sizeof(text)-1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_cursor_view core_before;
    boxrig_terminal_get_cursor(terminal, &core_before);
    boxrig_text_snapshot_options snapshot_options = {
        BOXRIG_TEXT_SCREEN_PRIMARY, NULL, 4096, 16};
    boxrig_text_snapshot *document_before = NULL;
    CHECK(boxrig_terminal_create_text_snapshot(terminal, &snapshot_options,
        &document_before) == BOXRIG_STATUS_OK);

    for (size_t backend = 0; backend < factory_count; backend++) {
        boxrig_test_color_renderer renderer = {0};
        CHECK(factories[backend](&renderer));
        /* The shared model requires equal cell geometry for both views. These
         * layout fixtures use the capture adapter's measured device cells. */
        boxrig_font_effective_metrics metrics = {0};
        metrics.requested_size_dip = 12;
        metrics.primary_face_key = 1;
        metrics.dpi_x = metrics.dpi_y = 96;
        metrics.output_scale = 1;
        metrics.glyph_width = renderer.cell_width;
        metrics.glyph_height = renderer.cell_height;
        metrics.ascent = renderer.cell_height;
        metrics.cell_width = renderer.cell_width;
        metrics.cell_height = renderer.cell_height;
        metrics.baseline = renderer.cell_height;
        metrics.layout_revision = backend+1;
        CHECK(boxrig_ui_view_commit_cell_metrics(views[0], &metrics) == BOXRIG_UI_OK);
        boxrig_ui_allocation_input allocation = {0};
        allocation.allocation_width = 12*renderer.cell_width*BOXRIG_UI_FIXED_ONE;
        allocation.allocation_height = 4*renderer.cell_height*BOXRIG_UI_FIXED_ONE;
        allocation.cell_width = renderer.cell_width*BOXRIG_UI_FIXED_ONE;
        allocation.cell_height = renderer.cell_height*BOXRIG_UI_FIXED_ONE;
        allocation.cell_metrics_revision = backend+1;
        for (unsigned i = 0; i < 2; i++) {
            boxrig_ui_layout_result layout;
            CHECK(boxrig_ui_view_accept_allocation(views[i], &allocation, &layout) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_invalidate(views[i], BOXRIG_UI_DIRTY_PRESENT,
                (boxrig_ui_dirty){0, 4, 0, 12}) == BOXRIG_UI_OK);
            CHECK(renderer.draw(renderer.context, views[i], NULL) == 0);
            if (backend == 0) {
                boxrig_ui_hit_test_result first, last;
                unsigned column = 1+2*i;
                CHECK(boxrig_ui_view_hit_test(views[i],
                    (column*renderer.cell_width+1)*BOXRIG_UI_FIXED_ONE,
                    BOXRIG_UI_FIXED_ONE, &first) == BOXRIG_UI_OK);
                CHECK(boxrig_ui_view_hit_test(views[i],
                    ((column+6)*renderer.cell_width-1)*BOXRIG_UI_FIXED_ONE,
                    BOXRIG_UI_FIXED_ONE, &last) == BOXRIG_UI_OK);
                CHECK(boxrig_ui_view_begin_selection(views[i],
                    BOXRIG_UI_SELECTION_CHARACTER, &first) == BOXRIG_UI_OK);
                CHECK(boxrig_ui_view_update_selection(views[i], &last) == BOXRIG_UI_OK);
                CHECK(boxrig_ui_view_create_selection_snapshot(views[i],
                    &copies[i]) == BOXRIG_UI_OK);
            }
            CHECK(check_pixels(&renderer, views[i], 1+2*i, &selections[i], &cursors[i]));
        }
        renderer.reset_statistics(renderer.context);
        for (unsigned change = 0; change < 24; change++) {
            selections[1].presentation = change%2 ? BOXRIG_UI_SELECTION_FILL_OUTLINE :
                BOXRIG_UI_SELECTION_FILL;
            selections[1].background_opacity = (uint8_t)(64+change*5);
            selections[1].marker_opacity = (uint8_t)(255-change*5);
            cursors[1].color = (boxrig_rgb){(uint8_t)(100+change), 215, 0};
            CHECK(boxrig_ui_view_set_selection_style(views[1], &selections[1]) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_set_cursor_style(views[1], &cursors[1]) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_set_focused(views[1], (change%2) != 0) == BOXRIG_UI_OK);
            CHECK(check_pixels(&renderer, views[1], 3, &selections[1], &cursors[1]));
            CHECK(check_pixels(&renderer, views[0], 1, &selections[0], &cursors[0]));
        }
        boxrig_test_color_statistics statistics;
        CHECK(renderer.statistics(renderer.context, &statistics) == 0);
        CHECK(statistics.row_shape_count == 0 && statistics.glyph_upload_count == 0);
        CHECK(check_cursor_boxes(&renderer, views[1]));
        for (unsigned i = 0; i < 2; i++) {
            boxrig_text_snapshot *copy = NULL;
            CHECK(boxrig_ui_view_create_selection_snapshot(views[i], &copy) == BOXRIG_UI_OK);
            CHECK(same_snapshot(copies[i], copy));
            boxrig_text_snapshot_destroy(copy);
        }
        renderer.destroy(renderer.context);
        printf("PASS %s shared model, independent views, 24 style updates, shapes=%zu uploads=%zu\n",
            names[backend], statistics.row_shape_count, statistics.glyph_upload_count);
    }
    boxrig_cursor_view core_after;
    boxrig_terminal_get_cursor(terminal, &core_after);
    CHECK(core_before.row == core_after.row && core_before.column == core_after.column &&
        core_before.shape == core_after.shape && core_before.visible == core_after.visible &&
        core_before.blinking == core_after.blinking);
    boxrig_text_snapshot *document_after = NULL;
    CHECK(boxrig_terminal_create_text_snapshot(terminal, &snapshot_options,
        &document_after) == BOXRIG_STATUS_OK);
    CHECK(same_snapshot(document_before, document_after));
    boxrig_text_snapshot_destroy(document_before);
    boxrig_text_snapshot_destroy(document_after);
    for (unsigned i = 0; i < 2; i++) {
        boxrig_text_snapshot_destroy(copies[i]);
        boxrig_ui_view_destroy(views[i]);
    }
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS renderer replacements retain decorations, Unicode document, selection copy and core cursor");
    return true;
}
