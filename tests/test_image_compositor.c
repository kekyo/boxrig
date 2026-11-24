/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/image.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static size_t blend_calls;

uint32_t __real_boxrig_ui_argb32_source_over(uint32_t source, uint32_t target);

/* Count general alpha blends independently of the machine's clock. */
uint32_t __wrap_boxrig_ui_argb32_source_over(uint32_t source, uint32_t target)
{
    blend_calls++;
    return __real_boxrig_ui_argb32_source_over(source, target);
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool premultiply_and_source_over_are_exact(void)
{
    static const uint8_t rgba[] = {200, 100, 50, 128};
    boxrig_image_view view;
    boxrig_premultiplied_image image;
    boxrig_image_composite_operation operation;
    uint32_t destination[] = {UINT32_C(0xff000000), UINT32_C(0xffffffff)};
    size_t pixel_count = 0;

    memset(&view, 0, sizeof(view));
    view.width = 1;
    view.height = 1;
    view.stride = sizeof(rgba);
    view.rgba = rgba;
    view.rgba_length = sizeof(rgba);
    memset(&image, 0, sizeof(image));
    CHECK(boxrig_premultiplied_image_create(&view, &image));
    CHECK(image.width == 1 && image.height == 1 && image.stride == 1);
    CHECK(image.pixels[0] == UINT32_C(0x80643219));

    memset(&operation, 0, sizeof(operation));
    operation.source_width = 1;
    operation.source_height = 1;
    operation.destination_width = 1;
    operation.destination_height = 1;
    operation.destination_clip_width = 1;
    operation.destination_clip_height = 1;
    operation.target_x = 0;
    operation.target_y = 0;
    operation.dirty.width = 2;
    operation.dirty.height = 1;
    CHECK(boxrig_composite_premultiplied_image(
        destination,
        2,
        1,
        2,
        &image,
        &operation,
        &pixel_count));
    CHECK(pixel_count == 1);
    CHECK(destination[0] == UINT32_C(0xff643219));

    operation.target_x = 1;
    CHECK(boxrig_composite_premultiplied_image(
        destination,
        2,
        1,
        2,
        &image,
        &operation,
        &pixel_count));
    CHECK(pixel_count == 1);
    CHECK(destination[1] == UINT32_C(0xffe3b198));
    boxrig_premultiplied_image_destroy(&image);
    return true;
}

static bool bilinear_scale_and_dirty_clip_are_exact(void)
{
    static const uint8_t rgba[] = {
        255, 0, 0, 255,
        0, 0, 255, 255
    };
    boxrig_image_view view;
    boxrig_premultiplied_image image;
    boxrig_image_composite_operation operation;
    uint32_t destination[] = {
        UINT32_C(0xff000000),
        UINT32_C(0xff000000),
        UINT32_C(0xff000000),
        UINT32_C(0xff000000)
    };
    size_t pixel_count = 0;

    memset(&view, 0, sizeof(view));
    view.width = 2;
    view.height = 1;
    view.stride = sizeof(rgba);
    view.rgba = rgba;
    view.rgba_length = sizeof(rgba);
    memset(&image, 0, sizeof(image));
    CHECK(boxrig_premultiplied_image_create(&view, &image));

    memset(&operation, 0, sizeof(operation));
    operation.source_width = 2;
    operation.source_height = 1;
    operation.destination_width = 4;
    operation.destination_height = 1;
    operation.destination_clip_width = 4;
    operation.destination_clip_height = 1;
    operation.dirty.width = 4;
    operation.dirty.height = 1;
    CHECK(boxrig_composite_premultiplied_image(
        destination,
        4,
        1,
        4,
        &image,
        &operation,
        &pixel_count));
    CHECK(pixel_count == 4);
    CHECK(destination[0] == UINT32_C(0xffff0000));
    CHECK(destination[1] == UINT32_C(0xffbf0040));
    CHECK(destination[2] == UINT32_C(0xff4000bf));
    CHECK(destination[3] == UINT32_C(0xff0000ff));

    memset(destination, 0, sizeof(destination));
    operation.destination_clip_x = 1;
    operation.destination_clip_width = 2;
    operation.target_x = 1;
    operation.dirty.x = 2;
    operation.dirty.width = 1;
    CHECK(boxrig_composite_premultiplied_image(
        destination,
        4,
        1,
        4,
        &image,
        &operation,
        &pixel_count));
    CHECK(pixel_count == 1);
    CHECK(destination[0] == 0 && destination[1] == 0 &&
          destination[2] == UINT32_C(0xff4000bf) && destination[3] == 0);
    boxrig_premultiplied_image_destroy(&image);
    return true;
}

static bool unscaled_crop_blends_only_visible_translucent_pixels(void)
{
    enum { source_stride = 8, target_stride = 7 };
    uint32_t source[6 * source_stride] = {0};
    boxrig_premultiplied_image image = {7, 6, source_stride, source};
    boxrig_image_composite_operation operation = {
        .source_x = 1, .source_y = 1, .source_width = 5, .source_height = 4,
        .destination_width = 5, .destination_height = 4,
        .destination_clip_x = 1, .destination_clip_y = 1,
        .destination_clip_width = 4, .destination_clip_height = 3,
        .target_x = -1, .target_y = -1, .dirty = {1, 0, 2, 2}
    };
    source[3 * source_stride + 5] = UINT32_C(0xff123456);
    source[4 * source_stride + 4] = UINT32_C(0x80402010);
    source[4 * source_stride + 5] = UINT32_C(0xff102030);
    blend_calls = 0;
    for (unsigned int behind = 0; behind <= 1; behind++) {
        uint32_t target[3 * target_stride], expected[3 * target_stride];
        for (size_t i = 0; i < 3 * target_stride; i++) target[i] = UINT32_C(0xff765432);
        target[2] = UINT32_C(0x80402010);
        target[target_stride + 1] = UINT32_C(0x60402008);
        target[target_stride + 2] = 0;
        memcpy(expected, target, sizeof(expected));
        expected[2] = behind ? UINT32_C(0xff493a3b) : UINT32_C(0xff123456);
        expected[target_stride + 1] = behind ? UINT32_C(0xb0683412) : UINT32_C(0xb0603014);
        expected[target_stride + 2] = UINT32_C(0xff102030);

        size_t count;
        bool ok = behind
            ? boxrig_composite_premultiplied_image_behind(target, 5, 3, target_stride, &image, &operation, &count)
            : boxrig_composite_premultiplied_image(target, 5, 3, target_stride, &image, &operation, &count);
        CHECK(ok && count == 4);
        CHECK(memcmp(target, expected, sizeof(target)) == 0);
    }
    printf("General alpha blends for two unscaled crops: %zu\n", blend_calls);
    CHECK(blend_calls <= 3);
    return true;
}

int main(void)
{
    if (!premultiply_and_source_over_are_exact() ||
        !bilinear_scale_and_dirty_clip_are_exact() ||
        !unscaled_crop_blends_only_visible_translucent_pixels()) return 1;
    printf("PASS portable image compositor premultiplies scales and blends\n");
    return 0;
}
