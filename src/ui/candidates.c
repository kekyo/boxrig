/* boxrig - Under MIT. */
#include "candidates_private.h"
#include "utf8.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void boxrig_ui_candidates_dispose(boxrig_ui_candidates *candidates)
{
    for (size_t i = 0; candidates->items != NULL && i < candidates->value.snapshot.count; i++)
        free(candidates->items[i]);
    for (size_t i = 0; candidates->labels != NULL && i < candidates->value.snapshot.page_size; i++)
        free(candidates->labels[i]);
    free(candidates->items);
    free(candidates->labels);
    free(candidates->auxiliary);
    free(candidates->auxiliary_above);
    memset(candidates, 0, sizeof(*candidates));
}

static boxrig_ui_status copy_text(const char *source, char **target, size_t *remaining)
{
    if (source == NULL) return BOXRIG_UI_OK;
    size_t length = 0;
    while (length < *remaining && source[length] != 0) length++;
    if (length == *remaining) return BOXRIG_UI_LIMIT_EXCEEDED;
    for (size_t offset = 0; offset < length;) {
        uint32_t scalar;
        size_t bytes;
        if (!boxrig_ui_utf8_decode_scalar((const uint8_t *)source + offset,
                length - offset, &scalar, &bytes)) return BOXRIG_UI_INVALID_ARGUMENT;
        offset += bytes;
    }
    *target = malloc(length + 1);
    if (*target == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    memcpy(*target, source, length + 1);
    *remaining -= length + 1;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_candidates_copy(boxrig_ui_candidates *target,
    const boxrig_ui_candidate_snapshot *source)
{
    if (source == NULL || source->count > BOXRIG_UI_CANDIDATES_MAXIMUM ||
        source->format < BOXRIG_UI_CANDIDATE_TABLE || source->format > BOXRIG_UI_CANDIDATE_PAGE ||
        (source->format == BOXRIG_UI_CANDIDATE_PAGE && source->count != source->page_size) ||
        source->page_size > BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM ||
        (source->count != 0 && (source->page_size == 0 || source->items == NULL)) ||
        (source->count == 0 ? source->cursor != 0 : source->cursor >= source->count) ||
        source->orientation < BOXRIG_UI_CANDIDATE_SYSTEM_ORIENTATION ||
        source->orientation > BOXRIG_UI_CANDIDATE_HORIZONTAL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_candidates next = {.value.snapshot = *source};
    if (source->count != 0) next.items = calloc(source->count, sizeof(*next.items));
    if (source->page_size != 0) next.labels = calloc(source->page_size, sizeof(*next.labels));
    boxrig_ui_status status = (source->count != 0 && next.items == NULL) ||
        (source->page_size != 0 && next.labels == NULL) ? BOXRIG_UI_OUT_OF_MEMORY : BOXRIG_UI_OK;
    size_t remaining = BOXRIG_UI_CANDIDATE_BYTES_MAXIMUM;
    for (size_t i = 0; status == BOXRIG_UI_OK && i < source->count; i++) {
        status = source->items[i] == NULL ? BOXRIG_UI_INVALID_ARGUMENT :
            copy_text(source->items[i], &next.items[i], &remaining);
    }
    for (size_t i = 0; status == BOXRIG_UI_OK && i < source->page_size; i++)
        status = copy_text(source->labels == NULL ? NULL : source->labels[i], &next.labels[i], &remaining);
    if (status == BOXRIG_UI_OK) status = copy_text(source->auxiliary, &next.auxiliary, &remaining);
    if (status == BOXRIG_UI_OK) status = copy_text(source->auxiliary_above, &next.auxiliary_above, &remaining);
    if (status != BOXRIG_UI_OK) { boxrig_ui_candidates_dispose(&next); return status; }
    next.value.snapshot.items = (const char *const *)next.items;
    next.value.snapshot.labels = (const char *const *)next.labels;
    next.value.snapshot.auxiliary = next.auxiliary;
    next.value.snapshot.auxiliary_above = next.auxiliary_above;
    next.value.revision = target->value.revision + 1;
    if (next.value.revision == 0) next.value.revision = 1;
    boxrig_ui_candidates_dispose(target);
    *target = next;
    return BOXRIG_UI_OK;
}

static int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }

boxrig_ui_status boxrig_ui_candidates_place(const boxrig_ui_candidate_view *candidates,
    const boxrig_ui_candidate_measurement *m, boxrig_ui_candidate_layout *layout)
{
    if (candidates == NULL || m == NULL || layout == NULL || m->width <= 0 || m->height <= 0 ||
        m->cell_width <= 0 || m->cell_width > INT_MAX / 64 ||
        m->line_height <= 0 || m->line_height > INT_MAX / 64 ||
        m->auxiliary_lines < 0 || m->auxiliary_lines > 2 || m->auxiliary_width < 0 ||
        m->auxiliary_above_lines < 0 || m->auxiliary_above_lines > 2 || m->auxiliary_above_width < 0)
        return BOXRIG_UI_INVALID_ARGUMENT;
    memset(layout, 0, sizeof(*layout));
    layout->revision = candidates->revision;
    const boxrig_ui_candidate_snapshot *s = &candidates->snapshot;
    bool above_visible = s->auxiliary_above_visible && s->auxiliary_above != NULL && s->auxiliary_above[0] != 0;
    bool below_visible = s->auxiliary_visible && s->auxiliary != NULL && s->auxiliary[0] != 0;
    if ((s->format == BOXRIG_UI_CANDIDATE_TABLE && s->count == 0) ||
        (!(s->visible && s->count != 0) && !above_visible && !below_visible)) return BOXRIG_UI_OK;
    size_t page = s->format == BOXRIG_UI_CANDIDATE_PAGE || s->count == 0 ? 0 : s->cursor / s->page_size * s->page_size;
    size_t count = s->visible ? s->count - page : 0;
    if (count > s->page_size) count = s->page_size;
    int32_t padding = maximum(2, m->cell_width / 2);
    int32_t row_height = m->line_height + padding * 2;
    int64_t horizontal_width = 2, width = 2;
    for (size_t i = 0; i < count; i++) {
        if (m->items[i] < 0) return BOXRIG_UI_INVALID_ARGUMENT;
        int64_t item_width = (int64_t)m->items[i] + padding * 2;
        if (item_width + 2 > width) width = item_width + 2;
        horizontal_width += item_width;
    }
    layout->horizontal = count != 0 && s->orientation == BOXRIG_UI_CANDIDATE_HORIZONTAL &&
        horizontal_width <= m->width;
    if (layout->horizontal) width = horizontal_width;
    if (below_visible && (int64_t)m->auxiliary_width + padding * 2 + 2 > width)
        width = (int64_t)m->auxiliary_width + padding * 2 + 2;
    if (above_visible && (int64_t)m->auxiliary_above_width + padding * 2 + 2 > width)
        width = (int64_t)m->auxiliary_above_width + padding * 2 + 2;
    int32_t panel_width = (int32_t)(width > m->width ? m->width : width);
    int32_t auxiliary_lines = below_visible ? m->auxiliary_lines : 0;
    int32_t above_lines = above_visible ? m->auxiliary_above_lines : 0;
    int32_t auxiliary_height = auxiliary_lines == 0 ? 0 : auxiliary_lines * m->line_height + padding * 2;
    int32_t above_height = above_lines == 0 ? 0 : above_lines * m->line_height + padding * 2;
    int32_t desired = (int32_t)(layout->horizontal ? count != 0 : count) * row_height + above_height + auxiliary_height + 2;
    int32_t above = minimum(m->height, maximum(0, m->anchor.y));
    int64_t anchor_bottom = (int64_t)m->anchor.y + m->anchor.height;
    int32_t below_start = (int32_t)(anchor_bottom < 0 ? 0 : anchor_bottom > m->height ? m->height : anchor_bottom);
    int32_t below = m->height - below_start;
    bool top = desired > below && above > below;
    int32_t available = top ? above : below;
    /* An extremely short viewport may cover the caret, but never puts the
     * selected row outside the viewport. It remains the keyboard-owned row. */
    if (available < row_height + 2) available = m->height;
    int32_t minimum_rows = count != 0 ? row_height : 0;
    while ((auxiliary_lines != 0 || above_lines != 0) && minimum_rows + above_height + auxiliary_height + 2 > available) {
        if (auxiliary_lines != 0) auxiliary_lines--;
        else above_lines--;
        auxiliary_height = auxiliary_lines == 0 ? 0 : auxiliary_lines * m->line_height + padding * 2;
        above_height = above_lines == 0 ? 0 : above_lines * m->line_height + padding * 2;
    }
    size_t visible = count;
    if (!layout->horizontal && count != 0) {
        int32_t capacity = (available - above_height - auxiliary_height - 2) / row_height;
        if (capacity < 1) capacity = 1;
        if (visible > (size_t)capacity) visible = (size_t)capacity;
    }
    layout->first = page;
    if (visible != 0 && s->cursor >= page + visible) layout->first = s->cursor - visible + 1;
    layout->count = visible;
    layout->hidden_before = layout->first > page;
    layout->hidden_after = layout->first + visible < page + count;
    layout->has_previous_page = s->format == BOXRIG_UI_CANDIDATE_PAGE ? s->has_previous_page : page != 0;
    layout->has_next_page = s->format == BOXRIG_UI_CANDIDATE_PAGE ? s->has_next_page : page + count < s->count;
    int32_t rows_height = (int32_t)(layout->horizontal ? visible != 0 : visible) * row_height;
    int32_t panel_height = minimum(available, above_height + rows_height + auxiliary_height + 2);
    int32_t x = minimum(maximum(0, m->anchor.x), m->width - panel_width);
    int32_t y = top ? above - panel_height : below_start;
    y = minimum(maximum(0, y), m->height - panel_height);
    layout->bounds = (boxrig_ui_candidate_rectangle){x, y, panel_width, panel_height};
    int32_t item_x = x + 1;
    for (size_t i = 0; i < visible; i++) {
        int32_t item_width = layout->horizontal ? m->items[i] + padding * 2 : maximum(0, panel_width - 2);
        int32_t item_y = y + 1 + above_height + (layout->horizontal ? 0 : (int32_t)i * row_height);
        layout->items[i] = (boxrig_ui_candidate_rectangle){item_x, item_y, item_width,
            maximum(0, minimum(row_height, y + panel_height - 1 - item_y))};
        if (layout->horizontal) item_x += item_width;
    }
    if (above_height != 0) layout->auxiliary_above = (boxrig_ui_candidate_rectangle){x + 1,
        y + 1, maximum(0, panel_width - 2), above_height};
    if (auxiliary_height != 0) layout->auxiliary = (boxrig_ui_candidate_rectangle){x + 1,
        y + 1 + above_height + rows_height, maximum(0, panel_width - 2), auxiliary_height};
    return BOXRIG_UI_OK;
}
