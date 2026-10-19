/* boxrig - Under MIT. */
#include "decoration_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static boxrig_ui_cursor_style demo_style(boxrig_ui_cursor_shape_policy shape)
{
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.block_presentation = BOXRIG_UI_CURSOR_BLOCK_FILL;
    style.shape = shape;
    if (shape != BOXRIG_UI_CURSOR_SHAPE_TERMINAL) {
        style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
        style.blink = shape == BOXRIG_UI_CURSOR_SHAPE_UNDERLINE
            ? BOXRIG_UI_CURSOR_BLINK_ENABLED : BOXRIG_UI_CURSOR_BLINK_STEADY;
        style.line_width_dip = shape == BOXRIG_UI_CURSOR_SHAPE_OUTLINE ? 2 : 3;
        style.color = shape == BOXRIG_UI_CURSOR_SHAPE_OUTLINE
            ? (boxrig_rgb){255, 215, 0} : shape == BOXRIG_UI_CURSOR_SHAPE_UNDERLINE
            ? (boxrig_rgb){0, 220, 255} : (boxrig_rgb){255, 80, 180};
    }
    return style;
}

static boxrig_ui_cursor_style box_demo_style(boxrig_ui_cursor_shape_policy shape)
{
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.shape = shape;
    style.presentation = BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED;
    style.blink = BOXRIG_UI_CURSOR_BLINK_ENABLED;
    style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
    style.color = (boxrig_rgb){255, 215, 0};
    return style;
}

int boxrig_term_decoration_demo_initialize(boxrig_ui_view *view)
{
    const char *value = getenv("BOXRIG_CURSOR_STYLE_DEMO");
    const char *selection = getenv("BOXRIG_SELECTION_STYLE_DEMO");
    if (value != NULL && strcmp(value, "0") != 0 && strcmp(value, "1") != 0 &&
        strcmp(value, "2") != 0) {
        fprintf(stderr, "boxrig_term: BOXRIG_CURSOR_STYLE_DEMO must be 0, 1 or 2\n");
        return -1;
    }
    if (selection != NULL && strcmp(selection, "0") != 0 && strcmp(selection, "1") != 0) {
        fprintf(stderr, "boxrig_term: BOXRIG_SELECTION_STYLE_DEMO must be 0 or 1\n");
        return -1;
    }
    boxrig_ui_cursor_style style = demo_style(value != NULL && strcmp(value, "1") == 0
        ? BOXRIG_UI_CURSOR_SHAPE_OUTLINE : BOXRIG_UI_CURSOR_SHAPE_TERMINAL);
    if (value != NULL && strcmp(value, "2") == 0)
        style = box_demo_style(BOXRIG_UI_CURSOR_SHAPE_BLOCK);
    if (boxrig_ui_view_set_cursor_style(view, &style) != BOXRIG_UI_OK) return -1;
    if (selection != NULL && strcmp(selection, "1") == 0 &&
        boxrig_ui_view_reset_selection_style(view) != BOXRIG_UI_OK) return -1;
    return 0;
}

boxrig_input_key_route boxrig_term_decoration_demo_route_key(
    boxrig_ui_view *view, const boxrig_key_event *event)
{
    if ((event->key != BOXRIG_KEY_F11 && event->key != BOXRIG_KEY_F10) ||
        (event->modifiers & ~(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK)) !=
            (BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT)) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    const bool selection = event->key == BOXRIG_KEY_F10;
    const char *value = getenv(selection ? "BOXRIG_SELECTION_STYLE_DEMO" : "BOXRIG_CURSOR_STYLE_DEMO");
    bool boxes = !selection && value != NULL && strcmp(value, "2") == 0;
    if (value == NULL || (strcmp(value, "1") != 0 && !boxes))
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (selection && event->action == BOXRIG_KEY_PRESS) {
        boxrig_ui_selection_style current;
        if (boxrig_ui_view_get_selection_style(view, &current) != BOXRIG_UI_OK)
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
        boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
        style.presentation = current.presentation == BOXRIG_UI_SELECTION_FILL_OUTLINE
            ? BOXRIG_UI_SELECTION_OUTLINE : current.presentation + 1;
        if (style.presentation != BOXRIG_UI_SELECTION_OUTLINE) {
            style.background = (boxrig_rgb){60,150,240};
            style.background_opacity = 96;
        }
        if (style.presentation == BOXRIG_UI_SELECTION_FILL_OUTLINE) {
            style.background = (boxrig_rgb){255,190,40};
            style.marker = (boxrig_rgb){255,215,0};
            style.marker_opacity = 220;
            style.line_width_dip = 2;
            style.foreground_policy = BOXRIG_UI_SELECTION_FOREGROUND_RGB;
            style.foreground = (boxrig_rgb){240,240,255};
        }
        return boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK
            ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION : BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    if (selection) return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    if (event->action == BOXRIG_KEY_PRESS) {
        boxrig_ui_cursor_style current;
        if (boxrig_ui_view_get_cursor_style(view, &current) != BOXRIG_UI_OK)
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
        boxrig_ui_cursor_shape_policy next = current.shape == BOXRIG_UI_CURSOR_SHAPE_BAR
            ? BOXRIG_UI_CURSOR_SHAPE_TERMINAL : current.shape + 1;
        boxrig_ui_cursor_style style = demo_style(next);
        if (boxes) {
            next = current.shape == BOXRIG_UI_CURSOR_SHAPE_BLOCK
                ? BOXRIG_UI_CURSOR_SHAPE_UNDERLINE : current.shape == BOXRIG_UI_CURSOR_SHAPE_UNDERLINE
                ? BOXRIG_UI_CURSOR_SHAPE_BAR : BOXRIG_UI_CURSOR_SHAPE_BLOCK;
            style = box_demo_style(next);
        }
        if (boxrig_ui_view_set_cursor_style(view, &style) != BOXRIG_UI_OK)
            return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}
