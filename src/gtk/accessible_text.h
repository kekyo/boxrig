/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_ACCESSIBLE_TEXT_H
#define BOXRIG_GTK_ACCESSIBLE_TEXT_H

#include "../ui/accessible.h"

typedef enum boxrig_gtk_accessible_event {
    BOXRIG_GTK_ACCESSIBLE_REMOVE,
    BOXRIG_GTK_ACCESSIBLE_INSERT,
    BOXRIG_GTK_ACCESSIBLE_CARET,
    BOXRIG_GTK_ACCESSIBLE_SELECTION,
    BOXRIG_GTK_ACCESSIBLE_ATTRIBUTES
} boxrig_gtk_accessible_event;

typedef struct boxrig_gtk_accessible_state {
    boxrig_ui_accessible_snapshot *snapshot;
    bool publishing;
    bool detached;
} boxrig_gtk_accessible_state;

typedef void (*boxrig_gtk_accessible_notify)(boxrig_gtk_accessible_event event,
    const boxrig_ui_accessible_change *change, void *context);

/* The frontend keeps its native object alive throughout synchronous callbacks.
 * A callback may detach it; no subsequent notification then escapes this call. */
boxrig_ui_status boxrig_gtk_accessible_publish(boxrig_gtk_accessible_state *state,
    boxrig_ui_view *view, boxrig_gtk_accessible_notify notify, void *context);
void boxrig_gtk_accessible_detach(boxrig_gtk_accessible_state *state);

/* Scalar, Unicode word/sentence, physical row, or hard-line paragraph.
 * Values 0..4 follow both GTK accessible-text granularity enumerations. */
bool boxrig_gtk_accessible_text_range(const boxrig_ui_accessible_snapshot *snapshot,
    size_t offset, unsigned int granularity, boxrig_ui_accessible_range *range);
bool boxrig_gtk_accessible_same_attributes(const boxrig_ui_accessible_character *left,
    const boxrig_ui_accessible_character *right);

#endif
