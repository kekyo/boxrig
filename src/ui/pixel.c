/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/pixel.h"

#include <stddef.h>

static uint8_t pixel_channel(uint32_t pixel, unsigned int shift)
{
    return (uint8_t)((pixel >> shift) & UINT32_C(0xff));
}

static uint8_t blend_channel(uint8_t source, uint8_t target, uint8_t alpha)
{
    unsigned int value = source +
                         ((unsigned int)target * (255u - alpha) + 127u) /
                             255u;

    return (uint8_t)(value < 255u ? value : 255u);
}

static bool background_role(boxrig_ui_pixel_role role)
{
    return role == BOXRIG_UI_PIXEL_ROLE_BACKGROUND ||
           role == BOXRIG_UI_PIXEL_ROLE_COMPOSITION_BACKGROUND;
}

static bool valid_role(boxrig_ui_pixel_role role)
{
    return role >= BOXRIG_UI_PIXEL_ROLE_BACKGROUND &&
           role <= BOXRIG_UI_PIXEL_ROLE_SELECTION_FOREGROUND;
}

bool boxrig_ui_background_opacity_from_percent(
    uint32_t percent,
    uint8_t *opacity_out)
{
    if (percent > 100 || opacity_out == NULL) {
        return false;
    }
    *opacity_out = (uint8_t)((percent * 255u + 50u) / 100u);
    return true;
}

uint32_t boxrig_ui_argb32_premultiply(
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha)
{
    uint32_t premultiplied_red =
        ((uint32_t)red * alpha + UINT32_C(127)) / UINT32_C(255);
    uint32_t premultiplied_green =
        ((uint32_t)green * alpha + UINT32_C(127)) / UINT32_C(255);
    uint32_t premultiplied_blue =
        ((uint32_t)blue * alpha + UINT32_C(127)) / UINT32_C(255);

    return ((uint32_t)alpha << 24) | (premultiplied_red << 16) |
           (premultiplied_green << 8) | premultiplied_blue;
}

uint32_t boxrig_ui_argb32_source_over(uint32_t source, uint32_t target)
{
    uint8_t alpha = pixel_channel(source, 24);
    uint8_t output_alpha = blend_channel(
        alpha,
        pixel_channel(target, 24),
        alpha);
    uint8_t output_red = blend_channel(
        pixel_channel(source, 16),
        pixel_channel(target, 16),
        alpha);
    uint8_t output_green = blend_channel(
        pixel_channel(source, 8),
        pixel_channel(target, 8),
        alpha);
    uint8_t output_blue = blend_channel(
        pixel_channel(source, 0),
        pixel_channel(target, 0),
        alpha);

    return ((uint32_t)output_alpha << 24) |
           ((uint32_t)output_red << 16) |
           ((uint32_t)output_green << 8) | output_blue;
}

uint32_t boxrig_ui_argb32_for_role(
    boxrig_ui_pixel_role role,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t source_alpha,
    uint8_t background_opacity)
{
    uint8_t alpha = source_alpha;

    if (!valid_role(role)) {
        return 0;
    }
    if (background_role(role)) {
        alpha = (uint8_t)(
            ((uint32_t)source_alpha * background_opacity + UINT32_C(127)) /
            UINT32_C(255));
    }
    return boxrig_ui_argb32_premultiply(red, green, blue, alpha);
}
