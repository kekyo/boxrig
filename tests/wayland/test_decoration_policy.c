/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "decoration_policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static void check_parse(
    const char *value,
    boxrig_term_wayland_decoration_preference expected)
{
    boxrig_term_wayland_decoration_preference preference =
        BOXRIG_TERM_WAYLAND_DECORATION_NONE;

    CHECK(boxrig_term_wayland_decoration_preference_parse(
              value,
              &preference) == BOXRIG_TERM_WAYLAND_DECORATION_OK);
    CHECK(preference == expected);
}

static void check_backend(
    boxrig_term_wayland_decoration_preference preference,
    bool server_available,
    bool libdecor_available,
    boxrig_term_wayland_decoration_status expected_status,
    boxrig_term_wayland_decoration_backend expected_backend)
{
    boxrig_term_wayland_decoration_backend backend =
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE;

    CHECK(boxrig_term_wayland_decoration_backend_resolve(
              preference,
              server_available,
              libdecor_available,
              &backend) == expected_status);
    if (expected_status == BOXRIG_TERM_WAYLAND_DECORATION_OK) {
        CHECK(backend == expected_backend);
    }
}

int main(void)
{
    boxrig_term_wayland_decoration_preference preference;
    boxrig_term_wayland_decoration_backend backend;

    check_parse(NULL, BOXRIG_TERM_WAYLAND_DECORATION_AUTO);
    check_parse("auto", BOXRIG_TERM_WAYLAND_DECORATION_AUTO);
    check_parse("server", BOXRIG_TERM_WAYLAND_DECORATION_SERVER);
    check_parse("client", BOXRIG_TERM_WAYLAND_DECORATION_CLIENT);
    check_parse("none", BOXRIG_TERM_WAYLAND_DECORATION_NONE);
    CHECK(boxrig_term_wayland_decoration_preference_parse(
              "",
              &preference) == BOXRIG_TERM_WAYLAND_DECORATION_INVALID);
    CHECK(boxrig_term_wayland_decoration_preference_parse(
              "SERVER",
              &preference) == BOXRIG_TERM_WAYLAND_DECORATION_INVALID);
    CHECK(boxrig_term_wayland_decoration_preference_parse(
              "system",
              &preference) == BOXRIG_TERM_WAYLAND_DECORATION_INVALID);
    CHECK(boxrig_term_wayland_decoration_preference_parse(
              NULL,
              NULL) == BOXRIG_TERM_WAYLAND_DECORATION_INVALID);

    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_AUTO,
        true,
        true,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_AUTO,
        true,
        false,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_AUTO,
        false,
        true,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_AUTO,
        false,
        false,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_SERVER,
        true,
        false,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_SERVER,
        false,
        true,
        BOXRIG_TERM_WAYLAND_DECORATION_UNAVAILABLE,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_CLIENT,
        true,
        true,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_CLIENT,
        false,
        false,
        BOXRIG_TERM_WAYLAND_DECORATION_UNAVAILABLE,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE);
    check_backend(
        BOXRIG_TERM_WAYLAND_DECORATION_NONE,
        true,
        true,
        BOXRIG_TERM_WAYLAND_DECORATION_OK,
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE);
    CHECK(boxrig_term_wayland_decoration_backend_resolve(
              (boxrig_term_wayland_decoration_preference)99,
              true,
              true,
              &backend) == BOXRIG_TERM_WAYLAND_DECORATION_INVALID);
    CHECK(boxrig_term_wayland_decoration_backend_resolve(
              BOXRIG_TERM_WAYLAND_DECORATION_AUTO,
              true,
              true,
              NULL) == BOXRIG_TERM_WAYLAND_DECORATION_INVALID);

    printf("PASS native Wayland sample decoration policy\n");
    return 0;
}
