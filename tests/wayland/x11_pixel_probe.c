/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_unsigned_long(const char *text, unsigned long *value_out)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value_out = value;
    return 0;
}

static int parse_coordinate(const char *text, int *value_out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 ||
        value > INT_MAX) {
        return -1;
    }
    *value_out = (int)value;
    return 0;
}

static unsigned int pixel_component(
    unsigned long pixel,
    unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long value;
    unsigned long maximum;

    if (mask == 0) {
        return 0;
    }
    while ((mask & 1u) == 0) {
        mask >>= 1;
        shift++;
    }
    value = pixel >> shift;
    maximum = mask;
    return (unsigned int)(((value & maximum) * 255u + maximum / 2u) /
                          maximum);
}

static int find_red_bounds(
    Display *display,
    Window window,
    const XWindowAttributes *attributes,
    int x,
    int y,
    int width,
    int height)
{
    XImage *image;
    int minimum_x = width;
    int minimum_y = height;
    int maximum_x = -1;
    int maximum_y = -1;
    int image_y;

    if (width <= 0 || height <= 0 || x > attributes->width - width ||
        y > attributes->height - height) {
        return 1;
    }
    image = XGetImage(
        display,
        window,
        x,
        y,
        (unsigned int)width,
        (unsigned int)height,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        return 1;
    }
    for (image_y = 0; image_y < height; image_y++) {
        int image_x;

        for (image_x = 0; image_x < width; image_x++) {
            unsigned long pixel = XGetPixel(image, image_x, image_y);
            unsigned int red = pixel_component(
                pixel,
                attributes->visual->red_mask);
            unsigned int green = pixel_component(
                pixel,
                attributes->visual->green_mask);
            unsigned int blue = pixel_component(
                pixel,
                attributes->visual->blue_mask);

            if (red >= 200u && green <= 64u && blue <= 64u) {
                if (image_x < minimum_x) {
                    minimum_x = image_x;
                }
                if (image_y < minimum_y) {
                    minimum_y = image_y;
                }
                if (image_x > maximum_x) {
                    maximum_x = image_x;
                }
                if (image_y > maximum_y) {
                    maximum_y = image_y;
                }
            }
        }
    }
    XDestroyImage(image);
    if (maximum_x < minimum_x || maximum_y < minimum_y) {
        return 1;
    }
    printf(
        "%d %d %d %d\n",
        x + minimum_x,
        y + minimum_y,
        maximum_x - minimum_x + 1,
        maximum_y - minimum_y + 1);
    return 0;
}

int main(int argc, char **argv)
{
    Display *display;
    XWindowAttributes attributes;
    XImage *image;
    unsigned long window_value;
    unsigned long pixel;
    int x;
    int y;
    int width;
    int height;
    bool find_red;
    bool hash_region;
    bool region;

    find_red = argc == 7 && strcmp(argv[1], "--find-red") == 0;
    hash_region = argc == 7 && strcmp(argv[1], "--hash") == 0;
    region = find_red || hash_region;
    if ((!region && argc != 4) ||
        parse_unsigned_long(argv[region ? 2 : 1], &window_value) == -1 ||
        parse_coordinate(argv[region ? 3 : 2], &x) == -1 ||
        parse_coordinate(argv[region ? 4 : 3], &y) == -1 ||
        (region &&
         (parse_coordinate(argv[5], &width) == -1 ||
          parse_coordinate(argv[6], &height) == -1 || width == 0 ||
          height == 0))) {
        fprintf(
            stderr,
            "usage: %s WINDOW X Y | "
            "%s --find-red|--hash WINDOW X Y WIDTH HEIGHT\n",
            argv[0],
            argv[0]);
        return 2;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL ||
        !XGetWindowAttributes(display, (Window)window_value, &attributes) ||
        x >= attributes.width || y >= attributes.height) {
        if (display != NULL) {
            XCloseDisplay(display);
        }
        return 1;
    }
    if (find_red) {
        int result = find_red_bounds(
            display,
            (Window)window_value,
            &attributes,
            x,
            y,
            width,
            height);

        XCloseDisplay(display);
        return result;
    }
    if (hash_region &&
        (x > attributes.width - width || y > attributes.height - height)) {
        XCloseDisplay(display);
        return 1;
    }
    image = XGetImage(
        display,
        (Window)window_value,
        x,
        y,
        hash_region ? (unsigned int)width : 1u,
        hash_region ? (unsigned int)height : 1u,
        AllPlanes,
        ZPixmap);
    if (image == NULL) {
        XCloseDisplay(display);
        return 1;
    }
    pixel = XGetPixel(image, 0, 0);
    if (hash_region) {
        uint64_t hash = UINT64_C(14695981039346656037);
        int row;
        int column;

        for (row = 0; row < height; row++) {
            for (column = 0; column < width; column++) {
                pixel = XGetPixel(image, column, row);
                hash ^= pixel_component(pixel, attributes.visual->red_mask);
                hash *= UINT64_C(1099511628211);
                hash ^= pixel_component(pixel, attributes.visual->green_mask);
                hash *= UINT64_C(1099511628211);
                hash ^= pixel_component(pixel, attributes.visual->blue_mask);
                hash *= UINT64_C(1099511628211);
            }
        }
        printf("%llu\n", (unsigned long long)hash);
    } else {
        printf(
            "%u %u %u\n",
            pixel_component(pixel, attributes.visual->red_mask),
            pixel_component(pixel, attributes.visual->green_mask),
            pixel_component(pixel, attributes.visual->blue_mask));
    }
    XDestroyImage(image);
    XCloseDisplay(display);
    return 0;
}
