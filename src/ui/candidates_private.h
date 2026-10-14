/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_CANDIDATES_PRIVATE_H
#define BOXRIG_UI_CANDIDATES_PRIVATE_H
#include "boxrig/ui/view.h"

/* The view owns these strings; public snapshots only borrow them. */
typedef struct boxrig_ui_candidates {
    boxrig_ui_candidate_view value;
    char **items;
    char **labels;
    char *auxiliary, *auxiliary_above, *panel_text, *panel_preedit;
} boxrig_ui_candidates;

void boxrig_ui_candidates_dispose(boxrig_ui_candidates *candidates);
boxrig_ui_status boxrig_ui_candidates_copy(boxrig_ui_candidates *target,
    const boxrig_ui_candidate_snapshot *source);

/* Measurements use the renderer's actual font. Placement never applies a
 * desktop origin, GTK scale factor, or font DPI to these pixels again. */
typedef struct boxrig_ui_candidate_measurement {
    int32_t width, height, cell_width, line_height;
    boxrig_ui_candidate_rectangle anchor;
    int32_t items[BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM];
    int32_t auxiliary_width, auxiliary_lines;
    int32_t auxiliary_above_width, auxiliary_above_lines;
    int32_t panel_text_width, panel_text_lines, panel_preedit_width;
} boxrig_ui_candidate_measurement;

boxrig_ui_status boxrig_ui_candidates_place(const boxrig_ui_candidate_view *candidates,
    const boxrig_ui_candidate_measurement *measurement, boxrig_ui_candidate_layout *layout);
void boxrig_ui_view_store_candidate_layout(boxrig_ui_view *view,
    const boxrig_ui_candidate_layout *layout);
#endif
