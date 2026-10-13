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

int main(void)
{
    CHECK(premultiply_and_source_over_are_exact());
    CHECK(bilinear_scale_and_dirty_clip_are_exact());
    printf("PASS portable image compositor premultiplies scales and blends\n");
    return 0;
}
