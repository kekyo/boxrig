/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/image.h"
#include "boxrig/ui/pixel.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0 && right > SIZE_MAX / left) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static bool multiply_divide_with_remainder(
    size_t left,
    size_t right,
    size_t divisor,
    size_t *quotient_out,
    size_t *remainder_out)
{
    size_t whole;
    size_t multiplicand;
    size_t remainder = 0;
    size_t fraction_quotient = 0;
    size_t mask = 1;

    if (divisor == 0) {
        return false;
    }
    whole = left / divisor;
    multiplicand = left % divisor;
    while (mask <= right / 2) {
        mask <<= 1;
    }
    for (; mask != 0; mask >>= 1) {
        size_t carry = 0;

        if (fraction_quotient > SIZE_MAX / 2) {
            return false;
        }
        fraction_quotient *= 2;
        if (remainder >= divisor - remainder) {
            remainder -= divisor - remainder;
            carry = 1;
        } else {
            remainder *= 2;
        }
        if (carry != 0) {
            fraction_quotient++;
        }
        if ((right & mask) != 0) {
            if (remainder >= divisor - multiplicand) {
                remainder -= divisor - multiplicand;
                if (fraction_quotient == SIZE_MAX) {
                    return false;
                }
                fraction_quotient++;
            } else {
                remainder += multiplicand;
            }
        }
    }
    if (whole != 0 && right > SIZE_MAX / whole) {
        return false;
    }
    whole *= right;
    if (fraction_quotient > SIZE_MAX - whole) {
        return false;
    }
    *quotient_out = whole + fraction_quotient;
    *remainder_out = remainder;
    return true;
}

static bool sample_coordinate(
    size_t destination_index,
    size_t destination_size,
    size_t source_size,
    size_t *coordinate_out)
{
    size_t scaled_source;
    size_t center_offset;
    size_t quotient;
    size_t remainder;
    size_t addition;
    size_t maximum;

    if (destination_index >= destination_size || source_size == 0 ||
        !multiply_size(source_size, 256, &scaled_source) ||
        !multiply_divide_with_remainder(
            destination_index,
            scaled_source,
            destination_size,
            &quotient,
            &remainder)) {
        return false;
    }
    center_offset = scaled_source / 2;
    if (quotient > SIZE_MAX - center_offset / destination_size) {
        return false;
    }
    quotient += center_offset / destination_size;
    addition = center_offset % destination_size;
    if (addition != 0) {
        if (remainder >= destination_size - addition) {
            remainder -= destination_size - addition;
            if (quotient == SIZE_MAX) {
                return false;
            }
            quotient++;
        } else {
            remainder += addition;
        }
    }
    if (quotient <= 128) {
        *coordinate_out = 0;
        return true;
    }
    quotient -= 128;
    if (!multiply_size(source_size - 1, 256, &maximum)) {
        return false;
    }
    *coordinate_out = quotient < maximum ? quotient : maximum;
    return true;
}

static uint8_t pixel_channel(uint32_t pixel, unsigned int shift)
{
    return (uint8_t)((pixel >> shift) & UINT32_C(0xff));
}

static uint8_t bilinear_channel(
    uint32_t top_left,
    uint32_t top_right,
    uint32_t bottom_left,
    uint32_t bottom_right,
    unsigned int shift,
    uint32_t horizontal,
    uint32_t vertical)
{
    uint32_t inverse_horizontal = 256 - horizontal;
    uint32_t inverse_vertical = 256 - vertical;
    uint32_t sum =
        (uint32_t)pixel_channel(top_left, shift) *
            inverse_horizontal * inverse_vertical +
        (uint32_t)pixel_channel(top_right, shift) *
            horizontal * inverse_vertical +
        (uint32_t)pixel_channel(bottom_left, shift) *
            inverse_horizontal * vertical +
        (uint32_t)pixel_channel(bottom_right, shift) *
            horizontal * vertical;

    return (uint8_t)((sum + UINT32_C(32768)) / UINT32_C(65536));
}

static uint32_t bilinear_pixel(
    const boxrig_premultiplied_image *image,
    size_t source_x,
    size_t source_y,
    size_t source_width,
    size_t source_height,
    size_t x_coordinate,
    size_t y_coordinate)
{
    size_t left = x_coordinate / 256;
    size_t top = y_coordinate / 256;
    size_t right = left + 1 < source_width ? left + 1 : left;
    size_t bottom = top + 1 < source_height ? top + 1 : top;
    uint32_t horizontal = (uint32_t)(x_coordinate % 256);
    uint32_t vertical = (uint32_t)(y_coordinate % 256);
    uint32_t top_left =
        image->pixels[(source_y + top) * image->stride + source_x + left];
    uint32_t top_right =
        image->pixels[(source_y + top) * image->stride + source_x + right];
    uint32_t bottom_left =
        image->pixels[(source_y + bottom) * image->stride + source_x + left];
    uint32_t bottom_right =
        image->pixels[(source_y + bottom) * image->stride + source_x + right];
    uint32_t alpha = bilinear_channel(
        top_left,
        top_right,
        bottom_left,
        bottom_right,
        24,
        horizontal,
        vertical);
    uint32_t red = bilinear_channel(
        top_left,
        top_right,
        bottom_left,
        bottom_right,
        16,
        horizontal,
        vertical);
    uint32_t green = bilinear_channel(
        top_left,
        top_right,
        bottom_left,
        bottom_right,
        8,
        horizontal,
        vertical);
    uint32_t blue = bilinear_channel(
        top_left,
        top_right,
        bottom_left,
        bottom_right,
        0,
        horizontal,
        vertical);

    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

bool boxrig_premultiplied_image_create(
    const boxrig_image_view *view,
    boxrig_premultiplied_image *image_out)
{
    boxrig_premultiplied_image image;
    size_t source_row_length;
    size_t source_required;
    size_t pixel_count;
    size_t row;

    if (view == NULL || image_out == NULL || view->rgba == NULL ||
        view->width == 0 || view->height == 0 ||
        !multiply_size(view->width, 4, &source_row_length) ||
        view->stride < source_row_length ||
        !multiply_size(view->height - 1, view->stride, &source_required) ||
        source_required > SIZE_MAX - source_row_length) {
        return false;
    }
    source_required += source_row_length;
    if (view->rgba_length < source_required ||
        !multiply_size(view->width, view->height, &pixel_count) ||
        pixel_count > SIZE_MAX / sizeof(*image.pixels)) {
        return false;
    }
    memset(&image, 0, sizeof(image));
    image.pixels = malloc(pixel_count * sizeof(*image.pixels));
    if (image.pixels == NULL) {
        return false;
    }
    image.width = view->width;
    image.height = view->height;
    image.stride = view->width;
    for (row = 0; row < view->height; row++) {
        const uint8_t *source = view->rgba + row * view->stride;
        size_t column;

        for (column = 0; column < view->width; column++) {
            image.pixels[row * image.stride + column] =
                boxrig_ui_argb32_premultiply(
                    source[column * 4],
                    source[column * 4 + 1],
                    source[column * 4 + 2],
                    source[column * 4 + 3]);
        }
    }
    *image_out = image;
    return true;
}

void boxrig_premultiplied_image_destroy(boxrig_premultiplied_image *image)
{
    if (image != NULL) {
        free(image->pixels);
        memset(image, 0, sizeof(*image));
    }
}

static bool composite_image(
    uint32_t *destination,
    size_t destination_width,
    size_t destination_height,
    size_t destination_stride,
    const boxrig_premultiplied_image *image,
    const boxrig_image_composite_operation *operation,
    size_t *pixel_count_out, bool behind)
{
    int64_t target_right;
    int64_t target_bottom;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    size_t dirty_right;
    size_t dirty_bottom;
    size_t pixel_count = 0;
    int64_t y;

    if (pixel_count_out == NULL) {
        return false;
    }
    *pixel_count_out = 0;
    if (destination == NULL || image == NULL || image->pixels == NULL ||
        operation == NULL || destination_width == 0 ||
        destination_height == 0 || destination_stride < destination_width ||
        destination_width > (size_t)INT64_MAX ||
        destination_height > (size_t)INT64_MAX ||
        operation->source_width == 0 || operation->source_height == 0 ||
        operation->source_x > image->width ||
        operation->source_width > image->width - operation->source_x ||
        operation->source_y > image->height ||
        operation->source_height > image->height - operation->source_y ||
        operation->destination_width == 0 ||
        operation->destination_height == 0 ||
        operation->destination_clip_x > operation->destination_width ||
        operation->destination_clip_width >
            operation->destination_width - operation->destination_clip_x ||
        operation->destination_clip_y > operation->destination_height ||
        operation->destination_clip_height >
            operation->destination_height - operation->destination_clip_y ||
        operation->destination_clip_width > (size_t)INT64_MAX ||
        operation->destination_clip_height > (size_t)INT64_MAX ||
        operation->target_x >
            INT64_MAX - (int64_t)operation->destination_clip_width ||
        operation->target_y >
            INT64_MAX - (int64_t)operation->destination_clip_height ||
        operation->dirty.x > destination_width ||
        operation->dirty.width > destination_width - operation->dirty.x ||
        operation->dirty.y > destination_height ||
        operation->dirty.height > destination_height - operation->dirty.y) {
        return false;
    }
    target_right = operation->target_x +
                   (int64_t)operation->destination_clip_width;
    target_bottom = operation->target_y +
                    (int64_t)operation->destination_clip_height;
    dirty_right = operation->dirty.x + operation->dirty.width;
    dirty_bottom = operation->dirty.y + operation->dirty.height;
    left = operation->target_x > (int64_t)operation->dirty.x
               ? operation->target_x
               : (int64_t)operation->dirty.x;
    top = operation->target_y > (int64_t)operation->dirty.y
              ? operation->target_y
              : (int64_t)operation->dirty.y;
    right = target_right < (int64_t)dirty_right
                ? target_right
                : (int64_t)dirty_right;
    bottom = target_bottom < (int64_t)dirty_bottom
                 ? target_bottom
                 : (int64_t)dirty_bottom;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > (int64_t)destination_width) {
        right = (int64_t)destination_width;
    }
    if (bottom > (int64_t)destination_height) {
        bottom = (int64_t)destination_height;
    }
    if (left >= right || top >= bottom) {
        return true;
    }
    for (y = top; y < bottom; y++) {
        size_t scaled_y = operation->destination_clip_y +
                          (size_t)(y - operation->target_y);
        size_t y_coordinate;
        int64_t x;

        if (!sample_coordinate(
                scaled_y,
                operation->destination_height,
                operation->source_height,
                &y_coordinate)) {
            return false;
        }
        for (x = left; x < right; x++) {
            size_t scaled_x = operation->destination_clip_x +
                              (size_t)(x - operation->target_x);
            size_t x_coordinate;
            uint32_t source;
            uint32_t *target;

            if (!sample_coordinate(
                    scaled_x,
                    operation->destination_width,
                    operation->source_width,
                    &x_coordinate)) {
                return false;
            }
            source = bilinear_pixel(
                image,
                operation->source_x,
                operation->source_y,
                operation->source_width,
                operation->source_height,
                x_coordinate,
                y_coordinate);
            target = &destination[(size_t)y * destination_stride + (size_t)x];
            *target = behind ? boxrig_ui_argb32_source_over(*target, source) :
                boxrig_ui_argb32_source_over(source, *target);
        }
        if ((size_t)(right - left) > SIZE_MAX - pixel_count) {
            return false;
        }
        pixel_count += (size_t)(right - left);
    }
    *pixel_count_out = pixel_count;
    return true;
}

bool boxrig_composite_premultiplied_image(uint32_t *destination,
    size_t width, size_t height, size_t stride, const boxrig_premultiplied_image *image,
    const boxrig_image_composite_operation *operation, size_t *count)
{
    return composite_image(destination, width, height, stride, image, operation, count, false);
}

bool boxrig_composite_premultiplied_image_behind(uint32_t *destination,
    size_t width, size_t height, size_t stride, const boxrig_premultiplied_image *image,
    const boxrig_image_composite_operation *operation, size_t *count)
{
    return composite_image(destination, width, height, stride, image, operation, count, true);
}
