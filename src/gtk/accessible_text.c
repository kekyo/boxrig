/* boxrig - Under MIT. */
#include "accessible_text.h"

#include <pango/pango.h>
#include <string.h>

bool boxrig_gtk_accessible_same_attributes(const boxrig_ui_accessible_character *left,
    const boxrig_ui_accessible_character *right)
{
    return left->attributes == right->attributes && left->underline == right->underline &&
        memcmp(&left->foreground, &right->foreground, sizeof(left->foreground)) == 0 &&
        memcmp(&left->background, &right->background, sizeof(left->background)) == 0;
}

boxrig_ui_status boxrig_gtk_accessible_publish(boxrig_gtk_accessible_state *state,
    boxrig_ui_view *view, bool enabled, boxrig_gtk_accessible_notify notify, void *context)
{
    if (state->detached) return BOXRIG_UI_UNAVAILABLE;
    if (state->publishing) return BOXRIG_UI_BUSY;
    if (!enabled && state->snapshot == NULL) return BOXRIG_UI_OK;
    boxrig_ui_accessible_snapshot *next = NULL;
    if (enabled) {
        boxrig_ui_status status = boxrig_ui_accessible_snapshot_create(view,
            BOXRIG_UI_ACCESSIBLE_MAXIMUM_BYTES, &next);
        if (status != BOXRIG_UI_OK) return status;
    }
    boxrig_ui_accessible_snapshot *old = state->snapshot;
    boxrig_ui_accessible_change change;
    boxrig_ui_accessible_snapshot_diff(old, next, &change);
    bool caret = old == NULL || old->caret != (next == NULL ? 0 : next->caret);
    size_t selection_count = next == NULL ? 0 : next->selection_count;
    bool selection = old == NULL ? selection_count != 0 :
        old->selection_count != selection_count ||
        (selection_count != 0 &&
        memcmp(old->selections, next->selections,
            selection_count * sizeof(*next->selections)) != 0);
    bool attributes = false;
    if (old != NULL && next != NULL && change.removed == 0 && change.inserted == 0) {
        for (size_t i = 0; i < next->character_count && !attributes; i++)
            attributes = !boxrig_gtk_accessible_same_attributes(&old->characters[i], &next->characters[i]);
    }
    state->publishing = true;
    if (change.removed != 0) notify(BOXRIG_GTK_ACCESSIBLE_REMOVE, &change, context);
    if (!state->detached) {
        state->snapshot = next;
        next = NULL;
        if (change.inserted != 0) notify(BOXRIG_GTK_ACCESSIBLE_INSERT, &change, context);
        if (!state->detached && caret) notify(BOXRIG_GTK_ACCESSIBLE_CARET, &change, context);
        if (!state->detached && selection) notify(BOXRIG_GTK_ACCESSIBLE_SELECTION, &change, context);
        if (!state->detached && attributes) notify(BOXRIG_GTK_ACCESSIBLE_ATTRIBUTES, &change, context);
    }
    if (state->snapshot == old) state->snapshot = NULL;
    boxrig_ui_accessible_snapshot_destroy(old);
    boxrig_ui_accessible_snapshot_destroy(next);
    state->publishing = false;
    if (state->detached) {
        boxrig_ui_accessible_snapshot_destroy(state->snapshot);
        state->snapshot = NULL;
    }
    return BOXRIG_UI_OK;
}

void boxrig_gtk_accessible_detach(boxrig_gtk_accessible_state *state)
{
    state->detached = true;
    if (!state->publishing) {
        boxrig_ui_accessible_snapshot_destroy(state->snapshot);
        state->snapshot = NULL;
    }
}

bool boxrig_gtk_accessible_text_range(const boxrig_ui_accessible_snapshot *snapshot,
    size_t offset, unsigned int granularity, boxrig_ui_accessible_range *range)
{
    if (snapshot == NULL || offset >= snapshot->character_count || granularity > 4) return false;
    if (granularity == 0) {
        *range = (boxrig_ui_accessible_range){offset, offset + 1};
        return true;
    }
    if (granularity == 3) {
        *range = snapshot->rows[snapshot->characters[offset].row];
        return true;
    }
    size_t first = offset, end = offset;
    while (first > 0 && snapshot->characters[first - 1].scalar != '\n') first--;
    while (end < snapshot->character_count && snapshot->characters[end].scalar != '\n') end++;
    if (end < snapshot->character_count) end++;
    if (granularity == 4) {
        *range = (boxrig_ui_accessible_range){first, end};
        return true;
    }
    size_t count = end - first;
    PangoLogAttr *attributes = g_try_new0(PangoLogAttr, count + 1);
    if (attributes == NULL) return false;
    size_t first_byte = snapshot->characters[first].byte_offset;
    pango_get_log_attrs(snapshot->text + first_byte,
        (int)(snapshot->characters[end].byte_offset - first_byte), -1,
        pango_language_get_default(), attributes, (int)(count + 1));
    size_t start = offset - first, limit = start + 1;
    while (start > 0 && !(granularity == 1 ? attributes[start].is_word_start : attributes[start].is_sentence_start))
        start--;
    while (limit < count && !(granularity == 1 ? attributes[limit].is_word_start : attributes[limit].is_sentence_start))
        limit++;
    g_free(attributes);
    *range = (boxrig_ui_accessible_range){first + start, first + limit};
    return true;
}
