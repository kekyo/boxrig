/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "../examples/boxrig_term/common/runtime_info.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(                                                         \
                stderr,                                                       \
                "check failed at %s:%d: %s\n",                              \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #condition);                                                  \
            return false;                                                     \
        }                                                                     \
    } while (0)

static bool read_stream(FILE *stream, char *bytes, size_t capacity)
{
    size_t length;

    CHECK(stream != NULL && bytes != NULL && capacity != 0);
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);
    length = fread(bytes, 1, capacity - 1, stream);
    CHECK(!ferror(stream));
    bytes[length] = '\0';
    return true;
}

static bool writes_available_runtime_information(void)
{
    static const char expected[] =
        "boxrig_term: frontend=x11\n"
        "boxrig_term: window-system=x11\n"
        "boxrig_term: renderer=opengl\n"
        "boxrig_term: graphics-api=OpenGL\n"
        "boxrig_term: graphics-api-version=4.6 Mock Driver\n"
        "boxrig_term: graphics-vendor=Mock Vendor\n"
        "boxrig_term: graphics-device=Mock Device\n";
    boxrig_term_runtime_info info = {
        "x11",
        "x11",
        "opengl",
        "OpenGL",
        "4.6 Mock Driver",
        "Mock Vendor",
        "Mock Device", NULL, NULL};
    char output[1024];
    FILE *stream = tmpfile();

    CHECK(stream != NULL);
    CHECK(boxrig_term_runtime_info_write(stream, &info) == 0);
    CHECK(read_stream(stream, output, sizeof(output)));
    CHECK(strcmp(output, expected) == 0);
    CHECK(fclose(stream) == 0);
    return true;
}

static bool omits_unavailable_runtime_information(void)
{
    static const char expected[] =
        "boxrig_term: frontend=win32\n"
        "boxrig_term: window-system=win32\n"
        "boxrig_term: renderer=win32\n"
        "boxrig_term: graphics-api=DirectWrite/GDI\n";
    boxrig_term_runtime_info info = {
        "win32",
        "win32",
        "win32",
        "DirectWrite/GDI",
        NULL,
        NULL,
        NULL, NULL, NULL};
    char output[1024];
    FILE *stream = tmpfile();

    CHECK(stream != NULL);
    CHECK(boxrig_term_runtime_info_write(stream, &info) == 0);
    CHECK(read_stream(stream, output, sizeof(output)));
    CHECK(strcmp(output, expected) == 0);
    CHECK(fclose(stream) == 0);
    return true;
}

static bool writes_direct3d_device_information(void)
{
    boxrig_term_runtime_info info = {
        .frontend = "win32", .window_system = "win32", .renderer = "d3d11",
        .graphics_api = "Direct3D 11/Direct2D", .graphics_vendor = "0x1414",
        .graphics_device = "Microsoft Basic Render Driver",
        .graphics_feature_level = "11_0", .graphics_driver_type = "warp"};
    char output[1024];
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    CHECK(boxrig_term_runtime_info_write(stream, &info) == 0);
    CHECK(read_stream(stream, output, sizeof(output)));
    CHECK(strstr(output, "boxrig_term: renderer=d3d11\n") != NULL);
    CHECK(strstr(output, "boxrig_term: graphics-feature-level=11_0\n") != NULL);
    CHECK(strstr(output, "boxrig_term: graphics-driver-type=warp\n") != NULL);
    CHECK(strstr(output, "graphics-api-version=") == NULL);
    CHECK(fclose(stream) == 0);
    return true;
}

int main(void)
{
    if (!writes_available_runtime_information() ||
        !omits_unavailable_runtime_information() ||
        !writes_direct3d_device_information()) return EXIT_FAILURE;
    puts("example runtime info tests passed");
    return EXIT_SUCCESS;
}
