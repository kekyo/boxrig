/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_PIXEL_H
#define BOXRIG_UI_PIXEL_H

#include <stdbool.h>
#include <stdint.h>

/** Semantic layer role controlling background-opacity application. */
typedef enum boxrig_ui_pixel_role {
    /** Grid, excess, default, explicit SGR, or reversed background. */
    BOXRIG_UI_PIXEL_ROLE_BACKGROUND = 0,
    /** Glyph foreground whose alpha represents authored coverage. */
    BOXRIG_UI_PIXEL_ROLE_FOREGROUND,
    /** Underline, strike, or another foreground decoration. */
    BOXRIG_UI_PIXEL_ROLE_DECORATION,
    /** Image pixel retaining its authored alpha. */
    BOXRIG_UI_PIXEL_ROLE_IMAGE,
    /** Cursor pixel retaining its cursor-policy alpha. */
    BOXRIG_UI_PIXEL_ROLE_CURSOR,
    /** Input-method composition background. */
    BOXRIG_UI_PIXEL_ROLE_COMPOSITION_BACKGROUND,
    /** Input-method composition glyph, underline, or caret. */
    BOXRIG_UI_PIXEL_ROLE_COMPOSITION_FOREGROUND,
    /** Selection fill retaining its style opacity independently of the window. */
    BOXRIG_UI_PIXEL_ROLE_SELECTION_BACKGROUND,
    /** Selected glyph foreground retaining its authored coverage. */
    BOXRIG_UI_PIXEL_ROLE_SELECTION_FOREGROUND
} boxrig_ui_pixel_role;

/**
 * Converts an inclusive percentage into deterministic eight-bit opacity.
 *
 * @param percent Percentage from zero through 100.
 * @param opacity_out Receives `(percent * 255 + 50) / 100` on success.
 * @return True for a valid percentage and output pointer; otherwise false.
 */
bool boxrig_ui_background_opacity_from_percent(
    uint32_t percent,
    uint8_t *opacity_out);

/**
 * Packs non-premultiplied channels into one premultiplied ARGB32 pixel.
 *
 * @param red Red channel.
 * @param green Green channel.
 * @param blue Blue channel.
 * @param alpha Source alpha.
 * @return Native-endian numeric 0xAARRGGBB with color channels multiplied by
 * alpha using `(channel * alpha + 127) / 255`.
 */
uint32_t boxrig_ui_argb32_premultiply(
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha);

/**
 * Source-over composites two premultiplied ARGB32 pixels.
 *
 * @param source Native-endian numeric premultiplied 0xAARRGGBB source.
 * @param target Native-endian numeric premultiplied 0xAARRGGBB target.
 * @return Premultiplied source-over result with deterministic integer rounding.
 */
uint32_t boxrig_ui_argb32_source_over(uint32_t source, uint32_t target);

/**
 * Produces a premultiplied pixel according to one terminal layer role.
 *
 * @param role Semantic layer role.
 * @param red Non-premultiplied red channel.
 * @param green Non-premultiplied green channel.
 * @param blue Non-premultiplied blue channel.
 * @param source_alpha Authored alpha or glyph coverage.
 * @param background_opacity Effective terminal background opacity.
 * @return Premultiplied 0xAARRGGBB, or zero for an invalid role.
 *
 * @remarks Background and composition-background roles multiply source_alpha
 * by background_opacity. Foreground, decoration, image, cursor, and selection
 * roles retain source_alpha unchanged.
 */
uint32_t boxrig_ui_argb32_for_role(
    boxrig_ui_pixel_role role,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t source_alpha,
    uint8_t background_opacity);

#endif
