/* boxrig - Under MIT. */
#include "preedit.h"
#include <stdlib.h>

static bool utf8_boundary(
    const char *text,
    size_t length,
    size_t offset)
{
    return offset <= length &&
           (offset == length ||
            (((const unsigned char *)text)[offset] & 0xc0) != 0x80);
}

static int compare_preedit_boundary(const void *left, const void *right)
{
    size_t a = *(const size_t *)left, b = *(const size_t *)right;
    return a < b ? -1 : a > b;
}

bool boxrig_ime_prepare_hints(const boxrig_ime_hint *hints, size_t hint_count,
    bool overflow, boxrig_composition_event *event,
    boxrig_composition_segment **segments, size_t *errors)
{
    size_t boundaries[2 * BOXRIG_IME_HINTS_MAXIMUM];
    boxrig_ime_hint valid[BOXRIG_IME_HINTS_MAXIMUM];
    size_t count = 0, boundary_count = 0, segment_count = 0;

    /* Decoration is optional. Exhausting its bound must not lose valid text
     * or a commit in this done batch, nor leave a misleading partial selection. */
    if (overflow) return true;
    for (size_t index = 0; index < hint_count; index++) {
        boxrig_ime_hint hint = hints[index];
        if (!utf8_boundary(event->text, event->text_length, hint.start) ||
            !utf8_boundary(event->text, event->text_length, hint.end)) {
            (*errors)++;
            continue;
        }
        valid[count++] = hint;
        boundaries[boundary_count++] = hint.start;
        boundaries[boundary_count++] = hint.end;
    }
    if (count == 0) return true;
    qsort(boundaries, boundary_count, sizeof(*boundaries), compare_preedit_boundary);
    (*segments) = calloc(boundary_count - 1, sizeof(*(*segments)));
    if ((*segments) == NULL) return false;
    for (size_t index = 1; index < boundary_count; index++) {
        size_t start = boundaries[index - 1], end = boundaries[index];
        uint32_t feedback = 0;
        if (start == end) continue;
        for (size_t hint = 0; hint < count; hint++)
            if (valid[hint].start <= start && end <= valid[hint].end)
                feedback |= valid[hint].feedback;
        if (feedback == BOXRIG_COMPOSITION_FEEDBACK_NONE) continue;
        boxrig_composition_segment *previous = segment_count == 0 ? NULL : &(*segments)[segment_count - 1];
        if (previous != NULL && previous->feedback == feedback &&
            previous->text_offset + previous->text_length == start) {
            previous->text_length += end - start;
        } else {
            (*segments)[segment_count++] = (boxrig_composition_segment){start, end - start, feedback};
        }
    }
    event->segments = (*segments);
    event->segment_count = segment_count;
    bool selected = false, disjoint = false;
    size_t selection_start = 0, selection_end = 0;
    for (size_t index = 0; index < segment_count; index++) {
        const boxrig_composition_segment *segment = &(*segments)[index];
        if ((segment->feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) == 0) continue;
        if (!selected) selection_start = segment->text_offset;
        else if (selection_end != segment->text_offset) disjoint = true;
        selection_end = segment->text_offset + segment->text_length;
        selected = true;
    }
    if (selected) {
        /* The common selection is contiguous. Separate target spans retain
         * their reverse feedback without selecting the intervening text. */
        event->selection_offset = disjoint ? 0 : selection_start;
        event->selection_length = disjoint ? 0 : selection_end - selection_start;
    }
    return true;
}
