/* boxrig - Under MIT. */
#include "background_demo.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int boxrig_term_background_effect_from_environment(boxrig_ui_background_effect_request *request)
{
    const char *value = getenv("BOXRIG_BACKGROUND_EFFECT");
    if (value == NULL || strcmp(value, "none") == 0) *request = BOXRIG_UI_BACKGROUND_EFFECT_NONE;
    else if (strcmp(value, "prefer") == 0) *request = BOXRIG_UI_BACKGROUND_EFFECT_PREFER;
    else if (strcmp(value, "require") == 0) *request = BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE;
    else {
        fprintf(stderr, "boxrig_term: BOXRIG_BACKGROUND_EFFECT must be none, prefer or require\n");
        return -1;
    }
    return 0;
}

static int initialize_color(boxrig_ui_view *view)
{
    const char *value = getenv("BOXRIG_BACKGROUND_RGBA");
    if (value == NULL) return 0;
    uint8_t channels[4];
    for (size_t i = 0; i < 4; i++) {
        if (*value < '0' || *value > '9') goto invalid;
        errno = 0;
        char *end;
        unsigned long channel = strtoul(value, &end, 10);
        if (errno != 0 || channel > 255 || *end != (i == 3 ? '\0' : ',')) goto invalid;
        channels[i] = (uint8_t)channel;
        value = end + (i == 3 ? 0 : 1);
    }
    return boxrig_ui_view_set_background_color(view,
        (boxrig_ui_rgba){channels[0], channels[1], channels[2], channels[3]}) == BOXRIG_UI_OK ? 0 : -1;
invalid:
    fprintf(stderr, "boxrig_term: BOXRIG_BACKGROUND_RGBA must contain r,g,b,a integers from 0 to 255\n");
    return -1;
}

int boxrig_term_background_demo_initialize(boxrig_ui_view *view)
{
    if (initialize_color(view) == -1) return -1;
    const char *fit = getenv("BOXRIG_BACKGROUND_IMAGE");
    if (fit == NULL) return 0;
    boxrig_ui_background_fit placement;
    if (strcmp(fit, "cover") == 0) placement = BOXRIG_UI_BACKGROUND_COVER;
    else if (strcmp(fit, "contain") == 0) placement = BOXRIG_UI_BACKGROUND_CONTAIN;
    else if (strcmp(fit, "stretch") == 0) placement = BOXRIG_UI_BACKGROUND_STRETCH;
    else {
        fprintf(stderr, "boxrig_term: BOXRIG_BACKGROUND_IMAGE must be cover, contain or stretch\n");
        return -1;
    }
    uint8_t rgba[64 * 32 * 4];
    for (size_t y = 0; y < 32; y++) for (size_t x = 0; x < 64; x++) {
        uint8_t *p = rgba + (y * 64 + x) * 4;
        bool alternate = ((x / 8) + (y / 8)) % 2 != 0;
        p[0] = alternate ? 24 : 160;
        p[1] = alternate ? 180 : 32;
        p[2] = alternate ? 220 : 140;
        p[3] = 255;
    }
    boxrig_ui_background_image image = {64, 32, 64 * 4, sizeof(rgba), rgba, 128, placement};
    return boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_OK ? 0 : -1;
}
