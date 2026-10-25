/* Preserve Pango's shaping clusters while fitting advances to terminal cells. */
#ifndef BOXRIG_PANGO_GLYPH_FIT_H
#define BOXRIG_PANGO_GLYPH_FIT_H

#include "../ui/glyph_fit.h"
#include <pango/pango.h>
#include <stdlib.h>

static boxrig_ui_glyph_span *boxrig_pango_glyph_spans(PangoGlyphItem *item,
    const char *text, const boxrig_text_layout *layout, size_t *count_out)
{
    *count_out = 0;
    if (item->glyphs->num_glyphs <= 0 ||
        (size_t)item->glyphs->num_glyphs > SIZE_MAX / sizeof(boxrig_ui_glyph_span)) return NULL;
    boxrig_ui_glyph_span *spans = calloc((size_t)item->glyphs->num_glyphs, sizeof(*spans));
    if (spans == NULL) return NULL;
    PangoGlyphItemIter iter;
    size_t count = 0;
    for (gboolean more = pango_glyph_item_iter_init_start(&iter, item, text);
         more; more = pango_glyph_item_iter_next_cluster(&iter)) {
        boxrig_ui_glyph_span *span = &spans[count++];
        span->text_offset = (size_t)iter.start_index;
        span->text_length = (size_t)(iter.end_index - iter.start_index);
        /* Pango uses visual glyph order, but the iterator walks logical text.
         * In RTL the inclusive start is greater than the exclusive end. */
        int first = iter.start_glyph < iter.end_glyph ? iter.start_glyph : iter.end_glyph + 1;
        int end = iter.start_glyph < iter.end_glyph ? iter.end_glyph : iter.start_glyph + 1;
        span->first_glyph = (size_t)first;
        span->glyph_count = (size_t)(end - first);
        for (int g = first; g < end; g++) span->natural_width += item->glyphs->glyphs[g].geometry.width;
    }
    *count_out = boxrig_ui_fit_glyph_spans(layout, spans, count);
    if (*count_out == 0) { free(spans); return NULL; }
    return spans;
}

#endif
