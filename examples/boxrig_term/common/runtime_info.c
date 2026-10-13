/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "runtime_info.h"

#include <errno.h>

static int write_field(FILE *output, const char *name, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    return fprintf(output, "boxrig_term: %s=%s\n", name, value) < 0 ? -1 : 0;
}

int boxrig_term_runtime_info_write(
    FILE *output,
    const boxrig_term_runtime_info *info)
{
    if (output == NULL || info == NULL || info->frontend == NULL ||
        info->frontend[0] == '\0' || info->window_system == NULL ||
        info->window_system[0] == '\0' || info->renderer == NULL ||
        info->renderer[0] == '\0' || info->graphics_api == NULL ||
        info->graphics_api[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (write_field(output, "frontend", info->frontend) == -1 ||
        write_field(output, "window-system", info->window_system) == -1 ||
        write_field(output, "renderer", info->renderer) == -1 ||
        write_field(output, "graphics-api", info->graphics_api) == -1 ||
        write_field(
            output,
            "graphics-api-version",
            info->graphics_api_version) == -1 ||
        write_field(output, "graphics-vendor", info->graphics_vendor) == -1 ||
        write_field(output, "graphics-device", info->graphics_device) == -1 ||
        write_field(output, "graphics-feature-level", info->graphics_feature_level) == -1 ||
        write_field(output, "graphics-driver-type", info->graphics_driver_type) == -1 ||
        fflush(output) == EOF) {
        return -1;
    }
    return 0;
}
