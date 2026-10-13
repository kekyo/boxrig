/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool image_regions_differ(
    XImage *image,
    unsigned int first_left,
    unsigned int second_left,
    unsigned int width,
    unsigned int height)
{
    unsigned int y;

    for (y = 0; y < height; y++) {
        unsigned int x;

        for (x = 0; x < width; x++) {
            if (XGetPixel(
                    image,
                    (int)(first_left + x),
                    (int)y) !=
                XGetPixel(
                    image,
                    (int)(second_left + x),
                    (int)y)) {
                return true;
            }
        }
    }
    return false;
}

static bool image_pixel_matches_rgb(
    Display *display,
    Colormap colormap,
    XImage *image,
    int x,
    int y,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    XColor color;

    memset(&color, 0, sizeof(color));
    color.pixel = XGetPixel(image, x, y);
    return XQueryColor(display, colormap, &color) != 0 &&
           color.red == (unsigned short)((unsigned int)red * 257u) &&
           color.green == (unsigned short)((unsigned int)green * 257u) &&
           color.blue == (unsigned short)((unsigned int)blue * 257u);
}

static uint8_t *read_font_file(const char *path, size_t *length_out)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (data == NULL ||
        fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length_out = (size_t)length;
    return data;
}

static bool effective_state_matches(
    const boxrig_font_effective_metrics *left,
    const boxrig_font_effective_metrics *right)
{
    return left->requested_size_dip == right->requested_size_dip &&
           left->primary_resource_id == right->primary_resource_id &&
           left->primary_face_key == right->primary_face_key &&
           left->dpi_x == right->dpi_x && left->dpi_y == right->dpi_y &&
           left->output_scale == right->output_scale &&
           left->glyph_width == right->glyph_width &&
           left->glyph_height == right->glyph_height &&
           left->ascent == right->ascent &&
           left->descent == right->descent &&
           left->line_gap == right->line_gap &&
           left->line_height_adjustment ==
               right->line_height_adjustment &&
           left->cell_width == right->cell_width &&
           left->cell_height == right->cell_height &&
           left->baseline == right->baseline;
}

static bool layout_positions_match(
    const boxrig_text_layout *left,
    const boxrig_text_layout *right)
{
    size_t column;
    size_t run_index;

    CHECK(boxrig_text_layout_column_count(left) ==
          boxrig_text_layout_column_count(right));
    CHECK(boxrig_text_layout_run_count(left) ==
          boxrig_text_layout_run_count(right));
    for (column = 0;
         column < boxrig_text_layout_column_count(left);
         column++) {
        size_t left_visual;
        size_t right_visual;
        size_t left_logical;
        size_t right_logical;

        CHECK(boxrig_text_layout_logical_to_visual(
                  left,
                  column,
                  &left_visual) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_logical_to_visual(
                  right,
                  column,
                  &right_visual) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(left_visual == right_visual);
        CHECK(boxrig_text_layout_visual_to_logical(
                  left,
                  column,
                  &left_logical) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_visual_to_logical(
                  right,
                  column,
                  &right_logical) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(left_logical == right_logical);
    }
    for (run_index = 0;
         run_index < boxrig_text_layout_run_count(left);
         run_index++) {
        boxrig_text_layout_run left_run;
        boxrig_text_layout_run right_run;

        CHECK(boxrig_text_layout_get_run(left, run_index, &left_run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_get_run(right, run_index, &right_run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        CHECK(left_run.logical_column == right_run.logical_column);
        CHECK(left_run.logical_column_count ==
              right_run.logical_column_count);
        CHECK(left_run.visual_column == right_run.visual_column);
        CHECK(left_run.visual_column_count == right_run.visual_column_count);
        CHECK(left_run.direction == right_run.direction);
        CHECK(left_run.bidi_level == right_run.bidi_level);
    }
    return true;
}

static bool configure_effective_metrics(boxrig_x11_renderer *renderer)
{
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    boxrig_font_visual_step_policy zoom_policy = {10.0f, 14.0f, 0.1f};
    boxrig_font_effective_metrics base;
    boxrig_font_effective_metrics dpi_144;
    boxrig_font_effective_metrics dpi_192;
    boxrig_font_effective_metrics fractional;
    boxrig_font_effective_metrics adjusted;
    boxrig_font_effective_metrics zoomed;
    boxrig_font_effective_metrics round_trip;
    boxrig_font_effective_metrics unchanged;
    bool changed = true;

    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &base) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(base.requested_size_dip == 12.0f);
    CHECK(base.primary_face_key != 0);
    CHECK(base.dpi_x == 96.0f && base.dpi_y == 96.0f);
    CHECK(base.output_scale == 1.0f);
    CHECK(base.glyph_width > 0.0f && base.glyph_height > 0.0f);
    CHECK(base.ascent > 0.0f && base.descent >= 0.0f);
    CHECK(base.line_gap >= 0.0f);
    CHECK(base.line_height_adjustment == 0.0f);
    CHECK(base.cell_width == boxrig_x11_renderer_cell_width(renderer));
    CHECK(base.cell_height == boxrig_x11_renderer_cell_height(renderer));
    CHECK(base.baseline > 0 && base.baseline <= base.cell_height);
    CHECK(base.layout_revision != 0);

    policy.dpi_x = 144.0f;
    policy.dpi_y = 144.0f;
    policy.antialias = BOXRIG_FONT_ANTIALIAS_GRAYSCALE;
    policy.hinting = BOXRIG_FONT_HINTING_DISABLED;
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &dpi_144) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(dpi_144.cell_width > base.cell_width);
    CHECK(dpi_144.cell_height > base.cell_height);

    policy.dpi_x = 192.0f;
    policy.dpi_y = 192.0f;
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &dpi_192) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(dpi_192.cell_width > dpi_144.cell_width);
    CHECK(dpi_192.cell_height > dpi_144.cell_height);

    policy.dpi_x = 96.0f;
    policy.dpi_y = 96.0f;
    policy.output_scale = 1.5f;
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &fractional) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(fractional.cell_width == dpi_144.cell_width);
    CHECK(fractional.cell_height == dpi_144.cell_height);
    CHECK(fractional.baseline == dpi_144.baseline);

    policy = boxrig_font_rendering_policy_default();
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &round_trip) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));
    CHECK(round_trip.layout_revision > base.layout_revision);

    CHECK(boxrig_x11_renderer_set_line_height_adjustment(renderer, 2.0f) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &adjusted) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(adjusted.requested_size_dip == round_trip.requested_size_dip);
    CHECK(adjusted.primary_face_key == round_trip.primary_face_key);
    CHECK(adjusted.glyph_width == round_trip.glyph_width);
    CHECK(adjusted.glyph_height == round_trip.glyph_height);
    CHECK(adjusted.cell_width == round_trip.cell_width);
    CHECK(adjusted.cell_height == round_trip.cell_height + 2);
    CHECK(adjusted.baseline == round_trip.baseline + 1);
    CHECK(adjusted.layout_revision == round_trip.layout_revision + 1);
    CHECK(boxrig_x11_renderer_set_line_height_adjustment(renderer, 0.0f) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &round_trip) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));

    CHECK(boxrig_x11_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &zoomed) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(zoomed.requested_size_dip > round_trip.requested_size_dip);
    CHECK(zoomed.cell_width != round_trip.cell_width);
    CHECK(zoomed.layout_revision == round_trip.layout_revision + 1);
    CHECK(boxrig_x11_renderer_visual_zoom_reset(renderer, &changed) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &base) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));
    changed = true;
    CHECK(boxrig_x11_renderer_visual_zoom_reset(renderer, &changed) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(!changed);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &unchanged) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(unchanged.layout_revision == base.layout_revision);

    CHECK(boxrig_x11_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_x11_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_DECREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &base) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));

    zoom_policy.minimum_size_dip = base.requested_size_dip;
    zoom_policy.maximum_size_dip = base.requested_size_dip;
    changed = true;
    CHECK(boxrig_x11_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(!changed);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &unchanged) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(unchanged.layout_revision == base.layout_revision);
    CHECK(effective_state_matches(&unchanged, &base));
    changed = true;
    CHECK(boxrig_x11_renderer_visual_zoom(
              renderer,
              (boxrig_font_zoom_direction)-1,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    CHECK(!changed);
    return true;
}

static bool configure_resource_fonts(
    boxrig_x11_renderer *renderer,
    const char *font_path)
{
    const uint8_t invalid_font[] = {0, 1, 2, 3};
    boxrig_font_resource_id file_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id memory_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id invalid_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    boxrig_font_family family;
    uint8_t *font_data;
    size_t font_length = 0;
    uint64_t revision;

    font_data = read_font_file(font_path, &font_length);
    CHECK(font_data != NULL);
    revision = boxrig_x11_renderer_font_catalog_revision(renderer);
    CHECK(revision != 0);
    CHECK(boxrig_x11_renderer_register_font_file(
              renderer,
              font_path,
              strlen(font_path),
              0,
              &file_id) == BOXRIG_FONT_CATALOG_OK);
    CHECK(file_id != BOXRIG_FONT_RESOURCE_NONE);
    CHECK(boxrig_x11_renderer_register_font_file(
              renderer,
              font_path,
              strlen(font_path),
              99,
              &invalid_id) == BOXRIG_FONT_CATALOG_INVALID_FONT);
    CHECK(invalid_id == BOXRIG_FONT_RESOURCE_NONE);
    CHECK(boxrig_x11_renderer_register_font_memory(
              renderer,
              font_data,
              font_length,
              0,
              &memory_id) == BOXRIG_FONT_CATALOG_OK);
    CHECK(memory_id != BOXRIG_FONT_RESOURCE_NONE && memory_id != file_id);
    CHECK(boxrig_x11_renderer_register_font_memory(
              renderer,
              invalid_font,
              sizeof(invalid_font),
              0,
              &invalid_id) == BOXRIG_FONT_CATALOG_INVALID_FONT);
    CHECK(boxrig_x11_renderer_font_catalog_revision(renderer) == revision + 2);

    memset(&family, 0, sizeof(family));
    family.resource_id = file_id;
    specification.families = &family;
    specification.family_count = 1;
    specification.fallback = BOXRIG_FONT_FALLBACK_REGISTERED_ONLY;
    specification.monospace = BOXRIG_FONT_MONOSPACE_UNRESTRICTED;
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_primary_font_resource(renderer) == file_id);
    CHECK(boxrig_x11_renderer_cell_width(renderer) > 0);
    CHECK(boxrig_x11_renderer_cell_height(renderer) > 0);

    family.resource_id = memory_id;
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_primary_font_resource(renderer) == memory_id);
    specification = boxrig_font_specification_default();
    policy = boxrig_font_rendering_policy_default();
    CHECK(boxrig_x11_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_primary_font_resource(renderer) ==
          BOXRIG_FONT_RESOURCE_NONE);
    CHECK(configure_effective_metrics(renderer));
    free(font_data);
    return true;
}

static bool render_terminal(Display *display)
{
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Window window;
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_render_statistics statistics;
    const uint8_t content[] = "\x1b[?25lA\x1b[31mB";
    const uint8_t decorations[] =
        "\x1b[H\x1b[31;4:1;58:2::0:255:0mA"
        "\x1b[0m\x1b[1;3H\x1b[4 q\x1b[?25h";
    const uint8_t erase[] = "\x1b[?25l\x1b[2J";
    const uint8_t dynamic_colors[] =
        "\x1b]4;1;#123456\x07"
        "\x1b]11;#654321\x1b\\"
        "\x1b[H\x1b[41mA\x1b[0mB";
    const uint8_t true_color[] =
        "\x1b[H\x1b[48;2;23;75;137mA\x1b[0m";
    const uint8_t unicode[] =
        "\x1b[?25l\x1b[2J\x1b[H"
        "e\xcc\x81"
        "?"
        "\xe7\x95\x8c";
    XImage *image;
    XColor underline_color;
    XColor palette_color;
    XColor default_background;
    unsigned int width;
    unsigned int height;
    unsigned long colors[16];
    size_t color_count = 0;
    size_t consumed = 0;
    unsigned int x;
    unsigned int y;

    window = XCreateSimpleWindow(
        display,
        root,
        0,
        0,
        160,
        80,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    width = boxrig_x11_renderer_cell_width(renderer) * 4;
    height = boxrig_x11_renderer_cell_height(renderer);
    CHECK(width > 0);
    CHECK(height > 0);
    XResizeWindow(display, window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, content, sizeof(content) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(content) - 1);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned long pixel = XGetPixel(image, (int)x, (int)y);
            size_t index;

            for (index = 0; index < color_count; index++) {
                if (colors[index] == pixel) {
                    break;
                }
            }
            if (index == color_count && color_count < 16) {
                colors[color_count] = pixel;
                color_count++;
            }
        }
    }
    CHECK(color_count >= 3);
    XDestroyImage(image);

    memset(&underline_color, 0, sizeof(underline_color));
    underline_color.green = 65535;
    underline_color.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(
              display,
              DefaultColormap(display, screen),
              &underline_color) != 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              decorations,
              sizeof(decorations) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(decorations) - 1);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(
              image,
              (int)boxrig_x11_renderer_cell_width(renderer) - 1,
              (int)height - 2) == underline_color.pixel);
    CHECK(XGetPixel(
              image,
              (int)(boxrig_x11_renderer_cell_width(renderer) * 2 +
                    boxrig_x11_renderer_cell_width(renderer) / 2),
              0) == XGetPixel(image, (int)width - 1, 0));
    CHECK(XGetPixel(
              image,
              (int)(boxrig_x11_renderer_cell_width(renderer) * 2 +
                    boxrig_x11_renderer_cell_width(renderer) / 2),
              (int)height - 2) != XGetPixel(image, (int)width - 1, 0));
    XDestroyImage(image);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              unicode,
              sizeof(unicode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(unicode) - 1);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(image_regions_differ(
        image,
        0,
        boxrig_x11_renderer_cell_width(renderer),
        boxrig_x11_renderer_cell_width(renderer),
        height));
    CHECK(image_regions_differ(
        image,
        boxrig_x11_renderer_cell_width(renderer) * 2,
        boxrig_x11_renderer_cell_width(renderer),
        boxrig_x11_renderer_cell_width(renderer),
        height));
    XDestroyImage(image);

    consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, erase, sizeof(erase) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            CHECK(XGetPixel(image, (int)x, (int)y) == XGetPixel(image, 0, 0));
        }
    }
    XDestroyImage(image);

    memset(&palette_color, 0, sizeof(palette_color));
    palette_color.red = 0x1212;
    palette_color.green = 0x3434;
    palette_color.blue = 0x5656;
    palette_color.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(
              display,
              DefaultColormap(display, screen),
              &palette_color) != 0);
    memset(&default_background, 0, sizeof(default_background));
    default_background.red = 0x6565;
    default_background.green = 0x4343;
    default_background.blue = 0x2121;
    default_background.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(
              display,
              DefaultColormap(display, screen),
              &default_background) != 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              dynamic_colors,
              sizeof(dynamic_colors) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(dynamic_colors) - 1);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(
              image,
              (int)boxrig_x11_renderer_cell_width(renderer) - 1,
              (int)height - 1) == palette_color.pixel);
    CHECK(XGetPixel(
              image,
              (int)(boxrig_x11_renderer_cell_width(renderer) * 2) - 1,
              (int)height - 1) == default_background.pixel);
    XDestroyImage(image);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              true_color,
              sizeof(true_color) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(true_color) - 1);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    if (DefaultVisual(display, screen)->class == TrueColor) {
        CHECK(statistics.server_color_round_trip_count == 0);
    }
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(image_pixel_matches_rgb(
        display,
        DefaultColormap(display, screen),
        image,
        (int)(boxrig_x11_renderer_cell_width(renderer) / 2),
        (int)height - 1,
        23,
        75,
        137));
    XDestroyImage(image);
    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool feed_text(boxrig_terminal *terminal, const uint8_t *text, size_t length)
{
    size_t consumed = 0;

    return boxrig_terminal_feed(terminal, text, length, &consumed) ==
               BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool cell_region_has_ink(
    XImage *image,
    unsigned long background,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t column)
{
    unsigned int y;

    for (y = 0; y < cell_height; y++) {
        unsigned int x;

        for (x = 0; x < cell_width; x++) {
            if (XGetPixel(
                    image,
                    (int)(column * cell_width + x),
                    (int)y) != background) {
                return true;
            }
        }
    }
    return false;
}

static bool composition_cell_has_ink(
    XImage *image,
    unsigned long background,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t row,
    size_t column)
{
    unsigned int y;

    for (y = 0; y < cell_height; y++) {
        unsigned int x;

        for (x = 0; x < cell_width; x++) {
            if (XGetPixel(
                    image,
                    (int)(column * cell_width + x),
                    (int)(row * cell_height + y)) != background) {
                return true;
            }
        }
    }
    return false;
}

static bool render_composition_overlay(Display *display)
{
    static const char preedit[] = "A\347\225\214";
    static const char feedback_preedit[] = "AAA";
    static const char long_preedit[] = "AAAA";
    static const char short_preedit[] = "A";
    static const char wrapping_preedit[] = "\347\225\214A";
    static const char rtl_preedit[] = "\327\220\327\221";
    static const char ltr_preedit[] = "AB";
    static const boxrig_composition_segment segments[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE},
        {1,
         3,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment feedback_segments[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {1, 1, BOXRIG_COMPOSITION_FEEDBACK_SECONDARY},
        {2, 1, BOXRIG_COMPOSITION_FEEDBACK_TERTIARY},
    };
    static const boxrig_composition_segment long_segment = {
        0,
        sizeof(long_preedit) - 1,
        BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
            BOXRIG_COMPOSITION_FEEDBACK_TARGET,
    };
    static const boxrig_composition_segment short_segment = {
        0,
        sizeof(short_preedit) - 1,
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE,
    };
    static const boxrig_composition_segment target_first[] = {
        {0,
         1,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
        {1, 2, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
    };
    static const boxrig_composition_segment target_middle[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {1,
         1,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
        {2, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
    };
    static const boxrig_composition_segment target_expanded[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {1,
         2,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment target_last[] = {
        {0, 2, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {2,
         1,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Window window = None;
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    boxrig_composition_cluster_view first_cluster;
    boxrig_composition_cluster_view second_cluster;
    XImage *image = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    unsigned long background;
    bool passed = false;

    window = XCreateSimpleWindow(
        display,
        root,
        0,
        0,
        160,
        80,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    cell_width = boxrig_x11_renderer_cell_width(renderer);
    cell_height = boxrig_x11_renderer_cell_height(renderer);
    width = cell_width * 8;
    height = cell_height * 3;
    XResizeWindow(display, window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[?25l\033[1;2H",
        sizeof("\033[?25l\033[1;2H") - 1));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = preedit;
    event.text_length = sizeof(preedit) - 1;
    event.segments = segments;
    event.segment_count = sizeof(segments) / sizeof(segments[0]);
    event.selection_offset = 1;
    event.selection_length = 3;
    event.caret_offset = sizeof(preedit) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.visible && view.candidate.visible);
    CHECK(view.candidate.column == 4);
    CHECK(view.candidate.spot_x == cell_width * 4);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    background = XGetPixel(image, 0, 0);
    CHECK(cell_region_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        1));
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 2 + cell_width / 2),
              (int)(cell_height / 2)) != background);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 1 + cell_width / 2),
              (int)cell_height - 2) != background);
    CHECK(XGetPixel(
              image,
              (int)view.candidate.spot_x,
              (int)(cell_height / 2)) != background);
    XDestroyImage(image);
    image = NULL;

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = feedback_preedit;
    event.text_length = sizeof(feedback_preedit) - 1;
    event.segments = feedback_segments;
    event.segment_count =
        sizeof(feedback_segments) / sizeof(feedback_segments[0]);
    event.caret_offset = sizeof(feedback_preedit) - 1;
    event.caret_visible = false;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(cell_width >= 4);
    CHECK(XGetPixel(
              image,
              (int)(cell_width + 1),
              (int)cell_height - 2) != background);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 2),
              (int)cell_height - 2) != background);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 2 + 1),
              (int)cell_height - 2) == background);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 3 + 2),
              (int)cell_height - 2) != background);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 3 + 3),
              (int)cell_height - 2) == background);
    XDestroyImage(image);
    image = NULL;

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 2 + cell_width / 2),
              (int)(cell_height / 2)) == background);
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 1 + cell_width / 2),
              (int)cell_height - 2) == background);
    XDestroyImage(image);
    image = NULL;

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = long_preedit;
    event.text_length = sizeof(long_preedit) - 1;
    event.segments = &long_segment;
    event.segment_count = 1;
    event.selection_length = sizeof(long_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        0,
        4));
    XDestroyImage(image);
    image = NULL;

    event.text = short_preedit;
    event.text_length = sizeof(short_preedit) - 1;
    event.segments = &short_segment;
    event.selection_length = 0;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        0,
        1));
    CHECK(!composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        0,
        2));
    CHECK(!composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        0,
        3));
    CHECK(!composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        0,
        4));
    XDestroyImage(image);
    image = NULL;

    event.text = feedback_preedit;
    event.text_length = sizeof(feedback_preedit) - 1;
    event.segments = target_first;
    event.segment_count = sizeof(target_first) / sizeof(target_first[0]);
    event.selection_offset = 0;
    event.selection_length = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, (int)cell_width + 1, 1) != background);
    CHECK(XGetPixel(image, (int)cell_width * 2 + 1, 1) == background);
    CHECK(XGetPixel(image, (int)cell_width * 3 + 1, 1) == background);
    XDestroyImage(image);
    image = NULL;

    event.segments = target_middle;
    event.segment_count = sizeof(target_middle) / sizeof(target_middle[0]);
    event.selection_offset = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, (int)cell_width + 1, 1) == background);
    CHECK(XGetPixel(image, (int)cell_width * 2 + 1, 1) != background);
    CHECK(XGetPixel(image, (int)cell_width * 3 + 1, 1) == background);
    XDestroyImage(image);
    image = NULL;

    event.segments = target_expanded;
    event.segment_count =
        sizeof(target_expanded) / sizeof(target_expanded[0]);
    event.selection_length = 2;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, (int)cell_width + 1, 1) == background);
    CHECK(XGetPixel(image, (int)cell_width * 2 + 1, 1) != background);
    CHECK(XGetPixel(image, (int)cell_width * 3 + 1, 1) != background);
    XDestroyImage(image);
    image = NULL;

    event.segments = target_last;
    event.segment_count = sizeof(target_last) / sizeof(target_last[0]);
    event.selection_offset = 2;
    event.selection_length = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, (int)cell_width + 1, 1) == background);
    CHECK(XGetPixel(image, (int)cell_width * 2 + 1, 1) == background);
    CHECK(XGetPixel(image, (int)cell_width * 3 + 1, 1) != background);
    XDestroyImage(image);
    image = NULL;

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[1;8H",
        sizeof("\033[1;8H") - 1));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = wrapping_preedit;
    event.text_length = sizeof(wrapping_preedit) - 1;
    event.caret_offset = sizeof(wrapping_preedit) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 1, &second_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(first_cluster.row == 1 && first_cluster.column == 0 &&
          first_cluster.column_count == 2);
    CHECK(second_cluster.row == 1 && second_cluster.column == 2 &&
          second_cluster.column_count == 1);
    CHECK(view.candidate.visible && view.candidate.row == 1 &&
          view.candidate.column == 3);
    CHECK(view.candidate.spot_x == cell_width * 3);
    CHECK(view.candidate.spot_y > cell_height &&
          view.candidate.spot_y < cell_height * 2);
    CHECK(view.candidate.exclude_x == cell_width * 3);
    CHECK(view.candidate.exclude_y == cell_height);
    CHECK(view.candidate.exclude_width == cell_width);
    CHECK(view.candidate.exclude_height == cell_height);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        1,
        0));
    CHECK(composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        1,
        2));
    CHECK(composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        1,
        3));
    XDestroyImage(image);
    image = NULL;

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[2J\033[H",
        sizeof("\033[2J\033[H") - 1));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = rtl_preedit;
    event.text_length = sizeof(rtl_preedit) - 1;
    event.selection_length = sizeof(rtl_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.direction == BOXRIG_TEXT_DIRECTION_LTR);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 1, &second_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(first_cluster.column < second_cluster.column);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, 1, 1) != background);
    CHECK(XGetPixel(image, (int)cell_width + 1, 1) != background);
    XDestroyImage(image);
    image = NULL;

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[2J\033[HA \327\220\327\221 B\033[1;3H",
        sizeof("\033[2J\033[HA \327\220\327\221 B\033[1;3H") - 1));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = ltr_preedit;
    event.text_length = sizeof(ltr_preedit) - 1;
    event.selection_length = sizeof(ltr_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.direction == BOXRIG_TEXT_DIRECTION_RTL);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 1, &second_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(first_cluster.row == second_cluster.row);
    CHECK(first_cluster.column > second_cluster.column);

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[2J\033[H\r\n\r\n\r\n",
        sizeof("\033[2J\033[H\r\n\r\n\r\n") - 1));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = long_preedit;
    event.text_length = sizeof(long_preedit) - 1;
    event.segments = &long_segment;
    event.segment_count = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    event.text = short_preedit;
    event.text_length = sizeof(short_preedit) - 1;
    event.segments = &short_segment;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && !view.visible);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.visible && view.cluster_count == 1);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column));
    CHECK(!composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column + 1));
    CHECK(!composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column + 2));
    CHECK(!composition_cell_has_ink(
        image,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column + 3));
    passed = true;

    XDestroyImage(image);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return passed;
}

static bool render_synchronized_output(Display *display)
{
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        80,
        40,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_render_statistics statistics;
    boxrig_cell_view cell;
    XColor marker;
    XImage *image;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    int marker_x;
    int marker_y;

    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    cell_width = boxrig_x11_renderer_cell_width(renderer);
    cell_height = boxrig_x11_renderer_cell_height(renderer);
    width = cell_width * 4;
    height = cell_height;
    XResizeWindow(display, window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[?25l\033[2J\033[HAAAA",
        sizeof("\033[?25l\033[2J\033[HAAAA") - 1));
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);

    memset(&marker, 0, sizeof(marker));
    marker.red = 65535;
    marker.green = 32768;
    marker.blue = 65535;
    marker.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(
              display,
              DefaultColormap(display, screen),
              &marker) != 0);
    marker_x = (int)(cell_width / 2);
    marker_y = (int)(cell_height / 2);
    XSetForeground(display, DefaultGC(display, screen), marker.pixel);
    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        marker_x,
        marker_y);
    XSync(display, False);

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[?2026h\033[HBBBB",
        sizeof("\033[?2026h\033[HBBBB") - 1));
    CHECK(boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "B", 1) == 0);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.cursor_raster_count == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) == marker.pixel);
    XDestroyImage(image);

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[HCCCC\033[?2026l",
        sizeof("\033[HCCCC\033[?2026l") - 1));
    CHECK(!boxrig_terminal_synchronized_output(terminal));
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 1);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 4);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) != marker.pixel);
    XDestroyImage(image);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "C", 1) == 0);

    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool render_incremental_dirty(Display *display)
{
    static const char preedit[] = "A";
    static const char updated_preedit[] = "AB";
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        160,
        80,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_x11_render_statistics statistics;
    XColor marker;
    XImage *image;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    int marker_x;
    int marker_y;

    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    cell_width = boxrig_x11_renderer_cell_width(renderer);
    cell_height = boxrig_x11_renderer_cell_height(renderer);
    width = cell_width * 8;
    height = cell_height * 3;
    XResizeWindow(display, window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);

    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.cursor_raster_count == 0);
    CHECK(statistics.composition_raster_count == 0);

    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[2;2H",
        sizeof("\033[2;2H") - 1));
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 2);
    CHECK(statistics.cell_raster_count == 2);
    CHECK(statistics.cursor_raster_count == 1);

    memset(&marker, 0, sizeof(marker));
    marker.red = 65535;
    marker.blue = 65535;
    marker.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(
              display,
              DefaultColormap(display, screen),
              &marker) != 0);
    marker_x = (int)(cell_width * 7 + cell_width / 2);
    marker_y = (int)(cell_height * 2 + cell_height / 2);
    XSetForeground(display, DefaultGC(display, screen), marker.pixel);
    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        marker_x,
        marker_y);
    XSync(display, False);

    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = preedit;
    event.text_length = sizeof(preedit) - 1;
    event.caret_offset = sizeof(preedit) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 2);
    CHECK(statistics.composition_raster_count == 1);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) == marker.pixel);
    XDestroyImage(image);

    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        marker_x,
        marker_y);
    event.text = updated_preedit;
    event.text_length = sizeof(updated_preedit) - 1;
    event.caret_offset = sizeof(updated_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 3);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) == marker.pixel);
    XDestroyImage(image);

    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        marker_x,
        marker_y);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 3);
    CHECK(statistics.composition_raster_count == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) == marker.pixel);
    XDestroyImage(image);

    CHECK(feed_text(
        terminal,
        (const uint8_t *)
            "\033[?25l\033[2J\033[HABCDEFG\033[31mA\033[0m",
        sizeof("\033[?25l\033[2J\033[HABCDEFG\033[31mA\033[0m") - 1));
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    marker_x = (int)(cell_width / 2);
    marker_y = (int)(cell_height / 2);
    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        marker_x,
        marker_y);
    CHECK(feed_text(
        terminal,
        (const uint8_t *)"\033[1;8H\033[31mD\033[0m",
        sizeof("\033[1;8H\033[31mD\033[0m") - 1));
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 1);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) == marker.pixel);
    XDestroyImage(image);

    marker_x = (int)(cell_width * 7 + cell_width / 2);
    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        marker_x,
        marker_y);
    CHECK(boxrig_x11_renderer_expose(
              renderer,
              (int)(cell_width * 7),
              0,
              cell_width,
              cell_height) == 0);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(XGetPixel(image, marker_x, marker_y) != marker.pixel);
    XDestroyImage(image);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool render_shared_model_views_independently(Display *display)
{
    static const uint8_t mixed[] =
        "\033[?25lA \327\220\327\221\327\222 B";
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        160,
        80,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_result layout;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_dirty dirty;
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL;
    boxrig_ui_view *second = NULL;
    unsigned int width;
    unsigned int height;
    size_t consumed = 0;

    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    CHECK(boxrig_x11_renderer_get_effective_metrics(renderer, &metrics) ==
          BOXRIG_FONT_CATALOG_OK);
    width = metrics.cell_width * 8;
    height = metrics.cell_height;
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XResizeWindow(display, window, width, height);
    XMapWindow(display, window);
    XSync(display, False);

    terminal_config.rows = 1;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_commit_cell_metrics(first, &metrics) == BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.allocation_width =
        (boxrig_ui_fixed)width * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(first, &input, &layout) ==
          BOXRIG_UI_OK);
    input.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_ui_view_accept_allocation(second, &input, &layout) ==
          BOXRIG_UI_OK);

    CHECK(boxrig_terminal_feed(
              terminal,
              mixed,
              sizeof(mixed) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(mixed) - 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_x11_renderer_draw_view(renderer, first) == 0);
    CHECK(!boxrig_ui_view_peek_dirty(
        first,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_IMAGE,
        &dirty));
    CHECK(boxrig_ui_view_hit_test(
              first,
              (boxrig_ui_fixed)(4 * metrics.cell_width) *
                      BOXRIG_UI_FIXED_ONE +
                  BOXRIG_UI_FIXED_ONE / 2,
              BOXRIG_UI_FIXED_ONE / 2,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.logical_column == 2);

    CHECK(boxrig_x11_renderer_draw_view(renderer, second) == 0);
    CHECK(!boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(!boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_IMAGE,
        &dirty));
    CHECK(boxrig_ui_view_hit_test(
              second,
              (boxrig_ui_fixed)(4 * metrics.cell_width) *
                      BOXRIG_UI_FIXED_ONE +
                  BOXRIG_UI_FIXED_ONE / 2,
              BOXRIG_UI_FIXED_ONE / 2,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.logical_column == 2);

    boxrig_ui_view_destroy(second);
    boxrig_ui_view_destroy(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool render_multilingual_layout(Display *display)
{
    const uint8_t mixed[] =
        "\033[?25lA \327\220\327\221\327\222 B";
    const uint8_t scripts[] =
        "\033[?25l\033[2J\033[H"
        "\330\263\331\204\330\247\331\205 "
        "\327\251\327\234\327\225\327\235 "
        "\340\244\250\340\244\256\340\244\270\340\245\215"
        "\340\244\244\340\245\207 "
        "\340\271\204\340\270\227\340\270\242 "
        "\347\225\214 e\314\201 "
        "\360\237\221\251\342\200\215\360\237\222\273";
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        640,
        80,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_text_layout *scaled_layout = NULL;
    boxrig_x11_shape_statistics statistics;
    boxrig_font_specification font_specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy rendering_policy =
        boxrig_font_rendering_policy_default();
    boxrig_cell_view before;
    boxrig_cell_view after;
    char before_text[8];
    const uint8_t *outbound = NULL;
    size_t visual;
    size_t logical;
    size_t run_index;
    bool found_rtl = false;
    unsigned int width;
    unsigned int height;
    XImage *image;
    unsigned long background;
    size_t inked_cells = 0;
    size_t column;

    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    width = boxrig_x11_renderer_cell_width(renderer) * 40;
    height = boxrig_x11_renderer_cell_height(renderer);
    XResizeWindow(display, window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 1;
    config.columns = 40;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, mixed, sizeof(mixed) - 1));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &before) ==
          BOXRIG_STATUS_OK);
    CHECK(before.text_length <= sizeof(before_text));
    memcpy(before_text, before.text, before.text_length);
    CHECK(boxrig_x11_renderer_layout_line(
              renderer,
              terminal,
              0,
              &layout,
              &statistics) == 0);
    CHECK(statistics.cluster_count > 0);
    CHECK(statistics.glyph_count > 0);
    CHECK(statistics.missing_glyph_count == 0);
    CHECK(statistics.font_run_count >= 2);
    CHECK(boxrig_text_layout_logical_to_visual(layout, 2, &visual) ==
          BOXRIG_TEXT_LAYOUT_OK);
    CHECK(visual == 4);
    CHECK(boxrig_text_layout_visual_to_logical(layout, visual, &logical) ==
          BOXRIG_TEXT_LAYOUT_OK);
    CHECK(logical == 2);
    for (run_index = 0;
         run_index < boxrig_text_layout_run_count(layout);
         run_index++) {
        boxrig_text_layout_run run;

        CHECK(boxrig_text_layout_get_run(layout, run_index, &run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        if (run.direction == BOXRIG_TEXT_DIRECTION_RTL &&
            run.logical_column <= 2 &&
            run.logical_column + run.logical_column_count >= 5) {
            found_rtl = true;
        }
    }
    CHECK(found_rtl);
    rendering_policy.dpi_x = 144.0f;
    rendering_policy.dpi_y = 144.0f;
    rendering_policy.output_scale = 1.25f;
    rendering_policy.antialias = BOXRIG_FONT_ANTIALIAS_NONE;
    CHECK(boxrig_x11_renderer_set_font(
              renderer,
              &font_specification,
              &rendering_policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_x11_renderer_layout_line(
              renderer,
              terminal,
              0,
              &scaled_layout,
              &statistics) == 0);
    CHECK(layout_positions_match(layout, scaled_layout));
    boxrig_text_layout_destroy(scaled_layout);
    scaled_layout = NULL;
    rendering_policy = boxrig_font_rendering_policy_default();
    CHECK(boxrig_x11_renderer_set_font(
              renderer,
              &font_specification,
              &rendering_policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &after) ==
          BOXRIG_STATUS_OK);
    CHECK(after.text_length == before.text_length);
    CHECK(memcmp(after.text, before_text, after.text_length) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &outbound) == 0);
    boxrig_text_layout_destroy(layout);
    layout = NULL;

    CHECK(boxrig_x11_renderer_set_direction_policy(
              renderer,
              BOXRIG_TEXT_DIRECTION_FORCE_RTL) == 0);
    CHECK(boxrig_x11_renderer_layout_line(
              renderer,
              terminal,
              0,
              &layout,
              &statistics) == 0);
    CHECK(boxrig_text_layout_logical_to_visual(layout, 0, &visual) ==
          BOXRIG_TEXT_LAYOUT_OK);
    CHECK(visual != 0);
    boxrig_text_layout_destroy(layout);
    layout = NULL;
    CHECK(boxrig_x11_renderer_set_direction_policy(
              renderer,
              BOXRIG_TEXT_DIRECTION_AUTO) == 0);

    CHECK(feed_text(terminal, scripts, sizeof(scripts) - 1));
    CHECK(boxrig_x11_renderer_layout_line(
              renderer,
              terminal,
              0,
              &layout,
              &statistics) == 0);
    CHECK(statistics.cluster_count >= 16);
    CHECK(statistics.glyph_count >= 16);
    CHECK(statistics.font_run_count >= 6);
    CHECK(statistics.missing_glyph_count == 0);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    background = XGetPixel(image, (int)width - 1, (int)height - 1);
    for (column = 0; column < 32; column++) {
        if (cell_region_has_ink(
                image,
                background,
                boxrig_x11_renderer_cell_width(renderer),
                height,
                column)) {
            inked_cells++;
        }
    }
    CHECK(inked_cells >= 12);
    XDestroyImage(image);
    CHECK(boxrig_terminal_output_peek(terminal, &outbound) == 0);

    boxrig_text_layout_destroy(layout);
    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool render_static_image_layers(Display *display)
{
    static const uint8_t setup[] =
        "\x1b[?25l"
        "\x1b]4;1;#ff0000\x07\x1b]10;#010203\x07\x1b]11;#000000\x07"
        "\x1b[1;2H\x1b[48;2;0;0;0m \x1b[0m"
        "\x1b[1;3H\x1b[41m \x1b[0m"
        "\x1b[1;4H\x1b[37;41mA\x1b[0m"
        "\x1b[1;7H\x1b[7m \x1b[0m"
        "\x1b_Ga=t,i=8001,f=32,s=1,v=1;AP8AgA==\x1b\\"
        "\x1b_Ga=t,i=8002,f=32,s=1,v=1;AAD/gA==\x1b\\"
        "\x1b_Ga=t,i=8020,f=32,s=1,v=1;/wD//w==\x1b\\"
        "\x1b_Ga=t,i=8010,f=32,s=1,v=1;//8A/w==\x1b\\"
        "\x1b_Ga=t,i=8004,f=32,s=2,v=1;/wAA/wAA//8=\x1b\\"
        "\x1b_Ga=t,i=42,f=32,s=1,v=1;AP///w==\x1b\\"
        "\x1b_Ga=t,i=8030,f=32,s=1,v=1;/4AA/w==\x1b\\"
        "\x1b[1;1H\x1b_Ga=p,i=8001,p=1,c=2,r=1,z=-2147483648,C=1\x1b\\"
        "\x1b[1;3H\x1b_Ga=p,i=8002,p=2,c=1,r=1,z=-1,C=1\x1b\\"
        "\x1b[1;4H\x1b_Ga=p,i=8020,p=20,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b[1;4H\x1b_Ga=p,i=8010,p=10,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b[1;5H\x1b_Ga=p,i=8004,p=4,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b_Ga=p,i=42,p=6,c=1,r=1,U=1,z=0,C=1\x1b\\"
        "\x1b[1;6H\x1b[38;5;42;58;5;6m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\x1b[0m"
        "\x1b[2;1H\x1b_Ga=p,i=8004,p=5,x=1,y=0,w=1,h=1,"
        "c=1,r=1,X=1,Y=1,z=0,C=1\x1b\\"
        "\x1b[2;2H\x1b_Ga=p,i=8004,p=40,x=0,y=0,w=1,h=1,"
        "c=1,r=1,z=1,C=1\x1b\\"
        "\x1b[2;2H\x1b_Ga=p,i=8004,p=41,x=1,y=0,w=1,h=1,"
        "c=1,r=1,z=1,C=1\x1b\\"
        "\x1b[2;3H\x1b_Ga=p,i=8030,p=30,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b[2;6H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,z=2,C=1;"
        "/wAA/w==\x1b\\"
        "\x1b[2;6H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,z=2,C=1;"
        "AP8A/w==\x1b\\"
        "\x1b[2;3H";
    static const uint8_t move[] =
        "\x1b[2;5H\x1b_Ga=p,i=8030,p=30,c=1,r=1,z=0,C=1\x1b\\";
    int screen = DefaultScreen(display);
    Colormap colormap = DefaultColormap(display, screen);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        320,
        120,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_render_statistics statistics;
    XImage *image = NULL;
    XColor marker;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;

    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    cell_width = boxrig_x11_renderer_cell_width(renderer);
    cell_height = boxrig_x11_renderer_cell_height(renderer);
    CHECK(cell_width > 1 && cell_height > 1);
    width = cell_width * 7;
    height = cell_height * 2;
    XResizeWindow(display, window, width, height);
    CHECK(boxrig_x11_renderer_set_surface_size(renderer, width, height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 2;
    config.columns = 7;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              cell_width,
              cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, setup, sizeof(setup) - 1));
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.image_cache_upload_count == 9);
    CHECK(statistics.image_composite_count >= 12);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width / 2),
        (int)(cell_height / 2),
        0,
        128,
        0));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width + cell_width / 2),
        (int)(cell_height / 2),
        0,
        0,
        0));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height / 2),
        127,
        0,
        128));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 3 + cell_width / 2),
        (int)(cell_height / 2),
        255,
        0,
        255));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 4),
        (int)(cell_height / 2),
        255,
        0,
        0));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 5 - 1),
        (int)(cell_height / 2),
        0,
        0,
        255));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 5 + cell_width / 2),
        (int)(cell_height / 2),
        0,
        255,
        255));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 6 + cell_width / 2),
        (int)(cell_height / 2),
        1,
        2,
        3));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        0,
        (int)(cell_height + 1),
        0,
        0,
        0));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        1,
        (int)(cell_height + 1),
        0,
        0,
        255));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        0,
        0,
        255));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 5 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        0,
        255,
        0));
    XDestroyImage(image);
    image = NULL;

    memset(&marker, 0, sizeof(marker));
    marker.red = 0x1212;
    marker.green = 0x3434;
    marker.blue = 0x5656;
    marker.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(display, colormap, &marker) != 0);
    XSetForeground(display, DefaultGC(display, screen), marker.pixel);
    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        (int)(cell_width * 6 + cell_width / 2),
        (int)(cell_height + cell_height / 2));
    CHECK(feed_text(terminal, move, sizeof(move) - 1));
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 3);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 1);
    CHECK(statistics.image_pixel_count ==
          (size_t)cell_width * cell_height);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        0,
        0,
        0));
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 4 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));
    CHECK(XGetPixel(
              image,
              (int)(cell_width * 6 + cell_width / 2),
              (int)(cell_height + cell_height / 2)) == marker.pixel);
    XDestroyImage(image);
    image = NULL;

    XDrawPoint(
        display,
        window,
        DefaultGC(display, screen),
        (int)(cell_width * 4 + cell_width / 2),
        (int)(cell_height + cell_height / 2));
    CHECK(boxrig_x11_renderer_expose(
              renderer,
              (int)(cell_width * 4),
              (int)cell_height,
              cell_width,
              cell_height) == 0);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 1);
    CHECK(statistics.image_pixel_count ==
          (size_t)cell_width * cell_height);
    XSync(display, False);
    image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    CHECK(image != NULL);
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width * 4 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));
    XDestroyImage(image);

    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

static bool render_animation_frames(Display *display)
{
    static const uint8_t setup[] =
        "\x1b[?25l"
        "\x1b_Ga=T,i=904,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=904,f=32,s=1,v=1,X=1,z=20;AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=904,r=1,z=10,s=3,v=1,c=1\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t green[] = {0, 255, 0, 255};
    int screen = DefaultScreen(display);
    Colormap colormap = DefaultColormap(display, screen);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        80,
        40,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    boxrig_x11_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_x11_render_statistics statistics;
    boxrig_image_handle handle = 0;
    boxrig_image_view image_view;
    boxrig_image_placement_view placement;
    XImage *image = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    uint64_t root_revision;

    CHECK(window != None);
    CHECK(boxrig_x11_renderer_create(display, window, &renderer) == 0);
    cell_width = boxrig_x11_renderer_cell_width(renderer);
    cell_height = boxrig_x11_renderer_cell_height(renderer);
    CHECK(cell_width > 0 && cell_height > 0);
    XResizeWindow(display, window, cell_width, cell_height);
    CHECK(boxrig_x11_renderer_set_surface_size(
              renderer,
              cell_width,
              cell_height) == 0);
    XMapWindow(display, window);
    XSync(display, False);

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              cell_width,
              cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, setup, sizeof(setup) - 1));
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image_view) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image_view.rgba, red, sizeof(red)) == 0);
    root_revision = image_view.revision;
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_handle == handle);
    CHECK(placement.visual_row == 0 && placement.visual_column == 0);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    XSync(display, False);
    image = XGetImage(
        display,
        window,
        0,
        0,
        cell_width,
        cell_height,
        AllPlanes,
        ZPixmap);
    CHECK(image != NULL);
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width / 2),
        (int)(cell_height / 2),
        255,
        0,
        0));
    XDestroyImage(image);
    image = NULL;

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 109) ==
          BOXRIG_STATUS_OK);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 0);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image_view) ==
          BOXRIG_STATUS_OK);
    CHECK(image_view.revision == root_revision);
    CHECK(memcmp(image_view.rgba, red, sizeof(red)) == 0);

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 110) ==
          BOXRIG_STATUS_OK);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    CHECK(statistics.image_cache_upload_count == 1);
    CHECK(statistics.image_composite_count == 1);
    CHECK(statistics.image_pixel_count ==
          (size_t)cell_width * cell_height);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image_view) ==
          BOXRIG_STATUS_OK);
    CHECK(image_view.revision != root_revision);
    CHECK(memcmp(image_view.rgba, green, sizeof(green)) == 0);
    XSync(display, False);
    image = XGetImage(
        display,
        window,
        0,
        0,
        cell_width,
        cell_height,
        AllPlanes,
        ZPixmap);
    CHECK(image != NULL);
    CHECK(image_pixel_matches_rgb(
        display,
        colormap,
        image,
        (int)(cell_width / 2),
        (int)(cell_height / 2),
        0,
        255,
        0));
    XDestroyImage(image);

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 110) ==
          BOXRIG_STATUS_OK);
    boxrig_x11_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_x11_renderer_draw(renderer, terminal) == 0);
    CHECK(boxrig_x11_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 0);

    boxrig_terminal_destroy(terminal);
    boxrig_x11_renderer_destroy(renderer);
    XDestroyWindow(display, window);
    XSync(display, False);
    return true;
}

int main(int argument_count, char **arguments)
{
    int display_pipe[2];
    pid_t server_process;
    char display_number[32];
    char display_name[40];
    size_t length = 0;
    Display *display;
    bool passed;
    int status;

    if (argument_count != 2) {
        fprintf(stderr, "font fixture path is required\n");
        return 1;
    }
    if (pipe(display_pipe) == -1) {
        perror("pipe");
        return 1;
    }
    server_process = fork();
    if (server_process == -1) {
        perror("fork");
        return 1;
    }
    if (server_process == 0) {
        char descriptor[16];

        close(display_pipe[0]);
        if (snprintf(descriptor, sizeof(descriptor), "%d", display_pipe[1]) < 0) {
            _exit(127);
        }
        execlp(
            "Xvfb",
            "Xvfb",
            "-displayfd",
            descriptor,
            "-screen",
            "0",
            "640x480x24",
            "-nolisten",
            "tcp",
            (char *)NULL);
        _exit(127);
    }
    close(display_pipe[1]);
    while (length + 1 < sizeof(display_number)) {
        ssize_t count = read(
            display_pipe[0],
            display_number + length,
            sizeof(display_number) - length - 1);

        if (count <= 0) {
            break;
        }
        length += (size_t)count;
        if (memchr(display_number, '\n', length) != NULL) {
            break;
        }
    }
    close(display_pipe[0]);
    if (length == 0) {
        kill(server_process, SIGTERM);
        waitpid(server_process, &status, 0);
        return 1;
    }
    display_number[strcspn(display_number, "\r\n")] = '\0';
    if (snprintf(display_name, sizeof(display_name), ":%s", display_number) < 0) {
        kill(server_process, SIGTERM);
        waitpid(server_process, &status, 0);
        return 1;
    }
    display = XOpenDisplay(display_name);
    if (display == NULL) {
        fprintf(stderr, "could not open Xvfb display %s\n", display_name);
        kill(server_process, SIGTERM);
        waitpid(server_process, &status, 0);
        return 1;
    }
    {
        int screen = DefaultScreen(display);
        Window root = RootWindow(display, screen);
        Window resource_window = XCreateSimpleWindow(
            display,
            root,
            0,
            0,
            32,
            32,
            0,
            BlackPixel(display, screen),
            BlackPixel(display, screen));
        boxrig_x11_renderer *resource_renderer = NULL;

        passed = resource_window != None &&
                 boxrig_x11_renderer_create(
                     display,
                     resource_window,
                     &resource_renderer) == 0 &&
                 configure_resource_fonts(resource_renderer, arguments[1]);
        boxrig_x11_renderer_destroy(resource_renderer);
        if (resource_window != None) {
            XDestroyWindow(display, resource_window);
        }
    }
    passed = passed && render_terminal(display) &&
             render_composition_overlay(display) &&
             render_synchronized_output(display) &&
             render_incremental_dirty(display) &&
             render_shared_model_views_independently(display) &&
             render_static_image_layers(display) &&
             render_animation_frames(display) &&
             render_multilingual_layout(display);
    /* Release Cairo's process-wide scaled-font cache after all renderers. */
    cairo_debug_reset_static_data();
    XCloseDisplay(display);
    FcFini();
    kill(server_process, SIGTERM);
    waitpid(server_process, &status, 0);
    if (!passed) {
        return 1;
    }
    printf("PASS X11 Pango renderer shapes multilingual terminal rows\n");
    return 0;
}
