/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_IMAGE_H
#define BOXRIG_UI_IMAGE_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Renderer-owned premultiplied ARGB image pixels. */
typedef struct boxrig_premultiplied_image {
    /** Pixel width. */
    size_t width;
    /** Pixel height. */
    size_t height;
    /** Number of uint32_t pixels between successive rows. */
    size_t stride;
    /** Native-endian 0xAARRGGBB premultiplied pixels. */
    uint32_t *pixels;
} boxrig_premultiplied_image;

/** Nonnegative pixel rectangle used to bound compositing work. */
typedef struct boxrig_pixel_rectangle {
    /** Left pixel. */
    size_t x;
    /** Top pixel. */
    size_t y;
    /** Pixel width. */
    size_t width;
    /** Pixel height. */
    size_t height;
} boxrig_pixel_rectangle;

/** One crop, scale, clip, and source-over operation. */
typedef struct boxrig_image_composite_operation {
    /** Left source pixel in the cached image. */
    size_t source_x;
    /** Top source pixel in the cached image. */
    size_t source_y;
    /** Source crop width. */
    size_t source_width;
    /** Source crop height. */
    size_t source_height;
    /** Full scaled destination width before clipping. */
    size_t destination_width;
    /** Full scaled destination height before clipping. */
    size_t destination_height;
    /** Left clip offset in the full scaled destination. */
    size_t destination_clip_x;
    /** Top clip offset in the full scaled destination. */
    size_t destination_clip_y;
    /** Visible destination width before dirty intersection. */
    size_t destination_clip_width;
    /** Visible destination height before dirty intersection. */
    size_t destination_clip_height;
    /** Signed target x corresponding to destination_clip_x. */
    int64_t target_x;
    /** Signed target y corresponding to destination_clip_y. */
    int64_t target_y;
    /** Additional target dirty clip. */
    boxrig_pixel_rectangle dirty;
} boxrig_image_composite_operation;

/**
 * Converts one core non-premultiplied RGBA image into cached ARGB pixels.
 *
 * @param view Immutable core image view.
 * @param image_out Receives renderer-owned pixels on success.
 * @return True on success; false for an invalid view or allocation failure.
 */
bool boxrig_premultiplied_image_create(
    const boxrig_image_view *view,
    boxrig_premultiplied_image *image_out);

/**
 * Releases pixels created by boxrig_premultiplied_image_create().
 *
 * @param image Image storage to release and clear. NULL is accepted.
 */
void boxrig_premultiplied_image_destroy(
    boxrig_premultiplied_image *image);

/**
 * Bilinearly scales and source-over composites one clipped image instance.
 *
 * @param destination Native-endian premultiplied 0xAARRGGBB target pixels.
 * @param destination_width Target pixel width.
 * @param destination_height Target pixel height.
 * @param destination_stride Target row stride in uint32_t pixels.
 * @param image Premultiplied source image.
 * @param operation Crop, scale, placement, and dirty parameters.
 * @param pixel_count_out Receives the number of target pixels processed.
 * @return True on success or an empty intersection; false for invalid input.
 *
 * @remarks Sampling uses eight fractional bits at destination pixel centers.
 * All channel interpolation and source-over rounding is platform independent.
 */
bool boxrig_composite_premultiplied_image(
    uint32_t *destination,
    size_t destination_width,
    size_t destination_height,
    size_t destination_stride,
    const boxrig_premultiplied_image *image,
    const boxrig_image_composite_operation *operation,
    size_t *pixel_count_out);

/**
 * Bilinearly composites an image behind existing target pixels.
 * @param destination Premultiplied ARGB32 target pixels.
 * @param width Target width in pixels.
 * @param height Target height in pixels.
 * @param stride Target row stride in pixels.
 * @param image Source pixels.
 * @param operation Crop, scale, placement and dirty clip.
 * @param count Receives processed pixel count.
 * @return True on success, including empty intersections.
 * @remarks Uses the same sampling and bounds as boxrig_composite_premultiplied_image().
 */
bool boxrig_composite_premultiplied_image_behind(uint32_t *destination,
    size_t width, size_t height, size_t stride, const boxrig_premultiplied_image *image,
    const boxrig_image_composite_operation *operation, size_t *count);

#endif
