/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_ACCESSIBLE_H
#define BOXRIG_UI_ACCESSIBLE_H

#include "boxrig/ui/view.h"

/* Private shared-frontend ABI. Includes all retained text, scalar, geometry,
 * row and selection storage, rather than only the UTF-8 payload. */
#define BOXRIG_UI_ACCESSIBLE_MAXIMUM_BYTES ((size_t)16 * 1024 * 1024)

typedef struct boxrig_ui_accessible_range {
    size_t start;
    size_t end;
} boxrig_ui_accessible_range;

typedef struct boxrig_ui_accessible_character {
    size_t byte_offset;
    size_t row;
    size_t column;
    size_t grapheme_start;
    size_t grapheme_end;
    boxrig_ui_fixed x, y, width, height;
    uint32_t scalar;
    uint32_t attributes;
    boxrig_underline_style underline;
    boxrig_rgb foreground, background;
    bool selected;
} boxrig_ui_accessible_character;

/* Immutable and independent of the originating terminal, view and layouts.
 * Characters are Unicode scalars. The extra terminal character contains the
 * final byte offset. Hard rows include LF, soft rows join without padding a
 * continuation cell. Unwritten trailing cells are omitted; explicit spaces
 * and indentation are retained. Carets outside the viewport clamp to its end. */
typedef struct boxrig_ui_accessible_snapshot {
    size_t memory_bytes;
    size_t byte_count;
    size_t character_count;
    size_t row_count;
    size_t caret;
    size_t selection_count;
    char *text;
    boxrig_ui_accessible_character *characters;
    boxrig_ui_accessible_range *rows;
    boxrig_ui_accessible_range *selections;
} boxrig_ui_accessible_snapshot;

typedef struct boxrig_ui_accessible_change {
    size_t start;
    size_t removed;
    size_t inserted;
} boxrig_ui_accessible_change;

/* The UI owner synchronizes the model first. Failure leaves *out NULL;
 * capacity exhaustion never publishes a truncated or partially mapped view. */
boxrig_ui_status boxrig_ui_accessible_snapshot_create(boxrig_ui_view *view,
    size_t maximum_bytes, boxrig_ui_accessible_snapshot **out);
void boxrig_ui_accessible_snapshot_destroy(boxrig_ui_accessible_snapshot *snapshot);

/* Minimal common-prefix/suffix replacement in Unicode scalar offsets.
 * NULL snapshots describe empty text. Neither input is modified. */
void boxrig_ui_accessible_snapshot_diff(const boxrig_ui_accessible_snapshot *old,
    const boxrig_ui_accessible_snapshot *next, boxrig_ui_accessible_change *change);

#endif
