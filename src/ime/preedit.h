/* boxrig - Under MIT. */
#ifndef BOXRIG_IME_PREEDIT_PRIVATE_H
#define BOXRIG_IME_PREEDIT_PRIVATE_H
#include "boxrig/ui/composition.h"

#define BOXRIG_IME_HINTS_MAXIMUM 128U
typedef struct boxrig_ime_hint {
    uint32_t start, end, feedback;
} boxrig_ime_hint;

/* Normalize optional byte-indexed decorations. The caller owns *segments.
 * Invalid decoration increments *errors without discarding valid text. */
bool boxrig_ime_prepare_hints(const boxrig_ime_hint *hints, size_t count,
    bool overflow, boxrig_composition_event *event,
    boxrig_composition_segment **segments, size_t *errors);
#endif
