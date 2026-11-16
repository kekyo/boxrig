/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "glyph_fit.h"

bool boxrig_ui_block_glyph_supported(uint32_t character)
{
    return (character >= 0x2580 && character <= 0x2590) ||
        (character >= 0x2594 && character <= 0x259f);
}

uint32_t boxrig_ui_block_glyph_utf8(const char *text, size_t length)
{
    if (text == NULL || length != 3 || (uint8_t)text[0] != 0xe2 ||
        (uint8_t)text[1] != 0x96 || (uint8_t)text[2] < 0x80 ||
        (uint8_t)text[2] > 0x9f) return 0;
    uint32_t character = 0x2580 + (uint8_t)text[2] - 0x80;
    return boxrig_ui_block_glyph_supported(character) ? character : 0;
}

static unsigned int boundary(unsigned int extent, unsigned int eighth)
{
    return (unsigned int)(((uint64_t)extent * eighth + 4) / 8);
}

static void append_rectangle(boxrig_ui_block_rectangle rectangles[3], size_t *count,
    unsigned int width, unsigned int height,
    unsigned int left, unsigned int top, unsigned int right, unsigned int bottom)
{
    unsigned int x = boundary(width, left), y = boundary(height, top);
    unsigned int end_x = boundary(width, right), end_y = boundary(height, bottom);
    if (x < end_x && y < end_y)
        rectangles[(*count)++] = (boxrig_ui_block_rectangle){x, y, end_x - x, end_y - y};
}

size_t boxrig_ui_block_glyph_rectangles(uint32_t character,
    unsigned int width, unsigned int height, boxrig_ui_block_rectangle rectangles[3])
{
    size_t count = 0;
    if (rectangles == NULL || width == 0 || height == 0 ||
        !boxrig_ui_block_glyph_supported(character)) return 0;
    unsigned int left = 0, top = 0, right = 8, bottom = 8;
    if (character == 0x2580) bottom = 4;
    else if (character <= 0x2588) top = 0x2588 - character;
    else if (character <= 0x258f) right = 0x2590 - character;
    else if (character == 0x2590) left = 4;
    else if (character == 0x2594) bottom = 1;
    else if (character == 0x2595) left = 7;
    else {
        /* Quadrant bits: upper left, upper right, lower left, lower right. */
        static const uint8_t quadrants[] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
        uint8_t mask = quadrants[character - 0x2596];
        for (unsigned int q = 0; q < 4; q++) {
            if ((mask & (1u << q)) == 0) continue;
            left = (q % 2) * 4;
            top = (q / 2) * 4;
            append_rectangle(rectangles, &count, width, height, left, top, left + 4, top + 4);
        }
        return count;
    }
    append_rectangle(rectangles, &count, width, height, left, top, right, bottom);
    return count;
}
