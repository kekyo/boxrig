/* Shared standard-scrollbar layout transaction - Under MIT. */
#ifndef BOXRIG_UI_SCROLLBAR_VISIBILITY_PRIVATE_H
#define BOXRIG_UI_SCROLLBAR_VISIBILITY_PRIVATE_H
#include "boxrig/ui/component.h"

typedef struct boxrig_ui_scrollbar_visibility_controller {
    boxrig_ui_scrollbar_visibility_state state;
    bool target_visible;
    unsigned int target_width;
    boxrig_ui_scrollbar_visibility previous_policy;
} boxrig_ui_scrollbar_visibility_controller;

void boxrig_ui_scrollbar_visibility_initialize(
    boxrig_ui_scrollbar_visibility_controller *controller,
    bool supported, unsigned int width);

/* full_allocation uses native buffer pixels. NULL means not allocated yet.
 * The caller commits native geometry only after the emitted resize succeeds.
 * Other frontend transactions must be excluded before calling this function. */
boxrig_ui_status boxrig_ui_scrollbar_visibility_prepare(
    boxrig_ui_scrollbar_visibility_controller *controller, boxrig_ui_view *view,
    boxrig_ui_scrollbar_visibility policy, bool supported, unsigned int width,
    const boxrig_ui_allocation_input *full_allocation);

void boxrig_ui_scrollbar_visibility_commit(
    boxrig_ui_scrollbar_visibility_controller *controller);

void boxrig_ui_scrollbar_visibility_cancel(
    boxrig_ui_scrollbar_visibility_controller *controller);
#endif
