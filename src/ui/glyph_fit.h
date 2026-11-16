/* Private, post-shaping placement shared by native text engines. */
#ifndef BOXRIG_UI_GLYPH_FIT_H
#define BOXRIG_UI_GLYPH_FIT_H

#include "boxrig/ui/text_layout.h"

typedef struct boxrig_ui_glyph_span {
    /* Offsets use the text engine's units, matching the portable layout. */
    size_t text_offset;
    size_t text_length;
    size_t first_glyph;
    size_t glyph_count;
    double natural_width;
    size_t visual_column;
    size_t column_count;
    bool preserve_advance;
    /* A complete, standalone Block Element, or zero for normal font shaping. */
    uint32_t block_character;
} boxrig_ui_glyph_span;

/* Cell-relative, nonoverlapping, opaque integer rectangles. */
typedef struct boxrig_ui_block_rectangle {
    unsigned int x, y, width, height;
} boxrig_ui_block_rectangle;

bool boxrig_ui_block_glyph_supported(uint32_t character);
uint32_t boxrig_ui_block_glyph_utf8(const char *text, size_t length);
/* The width covers the complete grapheme, including continuation columns.
 * Round shared eighth boundaries once; empty partitions at tiny sizes vanish. */
size_t boxrig_ui_block_glyph_rectangles(uint32_t character,
    unsigned int width, unsigned int height, boxrig_ui_block_rectangle rectangles[3]);

/* Input spans are native shaping clusters in logical text order. Merge any
 * that share a terminal grapheme, then adjacent spans with the same advance
 * per cell. Never split a ligature or reshape its contextual glyphs. Mutates
 * the caller's array and returns its reduced count, or zero on invalid input. */
size_t boxrig_ui_fit_glyph_spans(const boxrig_text_layout *layout,
    boxrig_ui_glyph_span *spans, size_t count);

#endif
