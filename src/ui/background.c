/* boxrig - Under MIT. */
#include "boxrig/ui/view.h"

boxrig_ui_background_rectangle boxrig_ui_background_image_place(
    boxrig_ui_background_image_view image, size_t width, size_t height)
{
    boxrig_ui_background_rectangle result = {0};
    if (image.pixels == NULL || image.width == 0 || image.height == 0 ||
        image.width > 16384 || image.height > 16384 || width == 0 || height == 0 ||
        width > INT32_MAX || height > INT32_MAX ||
        image.fit < BOXRIG_UI_BACKGROUND_COVER || image.fit > BOXRIG_UI_BACKGROUND_STRETCH) return result;
    uint64_t w = width, h = height;
    if (image.fit != BOXRIG_UI_BACKGROUND_STRETCH) {
        bool wider = (uint64_t)width * image.height > (uint64_t)height * image.width;
        bool use_width = image.fit == BOXRIG_UI_BACKGROUND_COVER ? wider : !wider;
        uint64_t rounding = image.fit == BOXRIG_UI_BACKGROUND_COVER ?
            (use_width ? image.width : image.height) - 1 : 0;
        if (use_width) h = ((uint64_t)width * image.height + rounding) / image.width;
        else w = ((uint64_t)height * image.width + rounding) / image.height;
        if (w == 0) w = 1;
        if (h == 0) h = 1;
    }
    if (w > SIZE_MAX || h > SIZE_MAX) return result;
    result.x = ((int64_t)width - (int64_t)w) / 2;
    result.y = ((int64_t)height - (int64_t)h) / 2;
    result.width = (size_t)w; result.height = (size_t)h;
    return result;
}
