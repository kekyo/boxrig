/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_BACKGROUND_DEMO_H
#define BOXRIG_TERM_BACKGROUND_DEMO_H
#include "boxrig/ui/view.h"
#include "boxrig/ui/background_effect.h"

/** Applies the optional numeric RGBA sample background; returns -1 on error. */
int boxrig_term_background_demo_initialize(boxrig_ui_view *view);
/** Reads the optional compositor effect request; returns -1 for invalid input. */
int boxrig_term_background_effect_from_environment(boxrig_ui_background_effect_request *request);
#endif
