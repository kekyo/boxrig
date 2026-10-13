/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "color_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static boxrig_ui_color_filter_result demo_color_filter(
    const boxrig_ui_color_filter_input *input, void *context)
{
    const boxrig_term_color_demo *demo = context;
    boxrig_ui_color_filter_result result =
        boxrig_ui_color_filter_result_default(input);
    if (demo->mode != 2) return result;
    if (input->target != BOXRIG_UI_COLOR_CELL &&
        input->target != BOXRIG_UI_COLOR_SURFACE &&
        input->target != BOXRIG_UI_COLOR_IME) {
        if (input->underlay_valid) {
            unsigned int brightness = 299u * input->underlay.background.red +
                587u * input->underlay.background.green +
                114u * input->underlay.background.blue;
            uint8_t marker = brightness >= 128000u ? 0 : 255;
            result.marker = (boxrig_rgb){marker, marker, marker};
        }
        return result;
    }
    /* This intentionally visible example depends on both original colors. */
    result.background.red = (uint8_t)((3u * input->colors.background.red +
        input->colors.foreground.red) / 4u);
    result.background.green = (uint8_t)((3u * input->colors.background.green +
        input->colors.foreground.green) / 4u);
    result.background.blue = (uint8_t)((3u * input->colors.background.blue +
        input->colors.foreground.blue) / 4u);
    if ((input->state_flags & BOXRIG_UI_COLOR_SELECTED) != 0) {
        result.background.blue = (uint8_t)((result.background.blue + 255u) / 2u);
    }
    unsigned int brightness = 299u * result.background.red +
        587u * result.background.green + 114u * result.background.blue;
    uint8_t foreground = brightness >= 128000u ? 0 : 255;
    result.foreground = (boxrig_rgb){foreground, foreground, foreground};
    if (input->underline_style != BOXRIG_UNDERLINE_NONE) {
        result.underline = (boxrig_rgb){255, 215, 0};
        result.underline_follows_foreground = false;
    }
    return result;
}

static boxrig_ui_status apply_color_demo(
    boxrig_term_color_demo *demo, boxrig_ui_view *view)
{
    return boxrig_ui_view_set_color_filter(
        view, demo->mode == 0 ? NULL : demo_color_filter, demo);
}

int boxrig_term_color_demo_initialize(
    boxrig_term_color_demo *demo, boxrig_ui_view *view)
{
    const char *value = getenv("BOXRIG_COLOR_FILTER_DEMO");

    demo->enabled = false;
    demo->mode = 0;
    if (value == NULL || strcmp(value, "0") == 0) return 0;
    if (strcmp(value, "1") != 0) {
        fprintf(stderr, "boxrig_term: BOXRIG_COLOR_FILTER_DEMO must be 0 or 1\n");
        return -1;
    }
    demo->mode = 2;
    if (apply_color_demo(demo, view) != BOXRIG_UI_OK) {
        demo->mode = 0;
        return -1;
    }
    demo->enabled = true;
    return 0;
}

boxrig_input_key_route boxrig_term_color_demo_route_key(
    boxrig_term_color_demo *demo, boxrig_ui_view *view,
    const boxrig_key_event *event)
{
    if (!demo->enabled || event->key != BOXRIG_KEY_F12 ||
        (event->modifiers & ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK)) !=
            (BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT)) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (event->action == BOXRIG_KEY_PRESS) {
        unsigned int previous = demo->mode;
        demo->mode = (previous + 1u) % 3u;
        if (apply_color_demo(demo, view) != BOXRIG_UI_OK) {
            demo->mode = previous;
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
        }
    }
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}
