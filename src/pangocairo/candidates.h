/* boxrig - Under MIT. */
#ifndef BOXRIG_PANGO_CANDIDATES_H
#define BOXRIG_PANGO_CANDIDATES_H
#include "../ui/candidates_private.h"
#include <pango/pangocairo.h>

/* A bounded page is shaped once per frame; no engine or terminal object is
 * retained here. Both CPU and GPU presentation use these exact placements. */
typedef struct boxrig_pango_candidate_frame {
    boxrig_ui_candidate_layout layout;
    PangoLayout *items[BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM];
    PangoLayout *auxiliary, *auxiliary_above, *panel_text, *panel_preedit;
    size_t page, selected;
    int padding;
    int preedit_scroll;
    bool cursor_visible;
} boxrig_pango_candidate_frame;

int boxrig_pango_candidates_prepare(PangoContext *context,
    const PangoFontDescription *font, const char *features, boxrig_font_fallback_policy fallback,
    const boxrig_ui_candidate_view *candidates, boxrig_ui_candidate_measurement measurement,
    boxrig_pango_candidate_frame *frame);
void boxrig_pango_candidates_dispose(boxrig_pango_candidate_frame *frame);
int boxrig_pango_candidates_draw(cairo_t *cr, const boxrig_pango_candidate_frame *frame,
    uint32_t foreground, uint32_t background);
#endif
