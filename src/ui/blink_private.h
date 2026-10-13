/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_BLINK_PRIVATE_H
#define BOXRIG_UI_BLINK_PRIVATE_H

#include "boxrig/ui/blink.h"

typedef struct boxrig_ui_blink_phase {
    bool visible;
    bool has_deadline;
    uint64_t deadline;
} boxrig_ui_blink_phase;

bool boxrig_ui_blink_config_valid(const boxrig_ui_blink_config *config);
/* Derive the present phase directly, without replaying missed cycles. */
bool boxrig_ui_blink_phase_at(boxrig_ui_blink_interval interval,
    uint64_t origin, uint64_t now, boxrig_ui_blink_phase *phase);

#endif
