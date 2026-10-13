/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/scrollbar.h"

#include <stdint.h>
#include <string.h>

static size_t scale_rounded(
    size_t value,
    size_t scale,
    size_t divisor)
{
    uint64_t product;

    if (divisor == 0 || value == 0 || scale == 0) {
        return 0;
    }
    product = (uint64_t)value * (uint64_t)scale;
    return (size_t)((product + (uint64_t)divisor / 2) / divisor);
}

int boxrig_scrollbar_model_read(
    boxrig_terminal *terminal,
    boxrig_scrollbar_model *model_out)
{
    size_t columns;
    boxrig_status active_status;

    if (terminal == NULL || model_out == NULL) {
        return -1;
    }
    memset(model_out, 0, sizeof(*model_out));
    boxrig_terminal_size(
        terminal,
        &model_out->viewport_rows,
        &columns);
    model_out->history_rows =
        boxrig_terminal_scrollback_rows(terminal);
    model_out->viewport_offset =
        boxrig_terminal_viewport_offset(terminal);
    active_status = boxrig_terminal_set_viewport_offset(
        terminal,
        model_out->viewport_offset);
    if (active_status != BOXRIG_STATUS_OK) {
        return 0;
    }
    if (model_out->viewport_offset > model_out->history_rows) {
        return -1;
    }
    model_out->position =
        model_out->history_rows - model_out->viewport_offset;
    model_out->enabled = model_out->history_rows != 0;
    return 0;
}

int boxrig_scrollbar_apply_action(
    boxrig_terminal *terminal,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out)
{
    boxrig_scrollbar_model model;
    size_t new_offset;

    if (terminal == NULL || changed_out == NULL ||
        action < BOXRIG_SCROLLBAR_LINE_UP ||
        action > BOXRIG_SCROLLBAR_POSITION) {
        return -1;
    }
    *changed_out = false;
    if (boxrig_scrollbar_model_read(terminal, &model) == -1) {
        return -1;
    }
    if (!model.enabled) {
        return 0;
    }
    new_offset = model.viewport_offset;
    switch (action) {
    case BOXRIG_SCROLLBAR_LINE_UP:
        if (new_offset < model.history_rows) {
            new_offset++;
        }
        break;
    case BOXRIG_SCROLLBAR_LINE_DOWN:
        if (new_offset != 0) {
            new_offset--;
        }
        break;
    case BOXRIG_SCROLLBAR_PAGE_UP:
        if (model.viewport_rows >= model.history_rows - new_offset) {
            new_offset = model.history_rows;
        } else {
            new_offset += model.viewport_rows;
        }
        break;
    case BOXRIG_SCROLLBAR_PAGE_DOWN:
        if (new_offset <= model.viewport_rows) {
            new_offset = 0;
        } else {
            new_offset -= model.viewport_rows;
        }
        break;
    case BOXRIG_SCROLLBAR_TOP:
        new_offset = model.history_rows;
        break;
    case BOXRIG_SCROLLBAR_BOTTOM:
        new_offset = 0;
        break;
    case BOXRIG_SCROLLBAR_POSITION:
        if (position > model.history_rows) {
            position = model.history_rows;
        }
        new_offset = model.history_rows - position;
        break;
    }
    if (new_offset == model.viewport_offset) {
        return 0;
    }
    if (boxrig_terminal_set_viewport_offset(terminal, new_offset) !=
        BOXRIG_STATUS_OK) {
        return -1;
    }
    *changed_out = true;
    return 0;
}

void boxrig_scrollbar_calculate_thumb(
    const boxrig_scrollbar_model *model,
    size_t track_length,
    size_t minimum_length,
    boxrig_scrollbar_thumb *thumb_out)
{
    size_t total_rows;
    size_t travel;

    if (thumb_out == NULL) {
        return;
    }
    thumb_out->start = 0;
    thumb_out->length = track_length;
    if (model == NULL || track_length == 0 || !model->enabled ||
        model->history_rows == 0) {
        return;
    }
    total_rows = model->history_rows + model->viewport_rows;
    if (total_rows < model->history_rows || total_rows == 0) {
        return;
    }
    thumb_out->length = (size_t)(
        ((uint64_t)track_length * (uint64_t)model->viewport_rows) /
        (uint64_t)total_rows);
    if (thumb_out->length == 0) {
        thumb_out->length = 1;
    }
    if (thumb_out->length < minimum_length) {
        thumb_out->length = minimum_length;
    }
    if (thumb_out->length > track_length) {
        thumb_out->length = track_length;
    }
    travel = track_length - thumb_out->length;
    thumb_out->start = scale_rounded(
        model->position > model->history_rows
            ? model->history_rows
            : model->position,
        travel,
        model->history_rows);
}

size_t boxrig_scrollbar_position_from_thumb(
    const boxrig_scrollbar_model *model,
    size_t track_length,
    size_t minimum_length,
    size_t thumb_start)
{
    boxrig_scrollbar_thumb thumb;
    size_t travel;

    if (model == NULL || !model->enabled || model->history_rows == 0) {
        return 0;
    }
    boxrig_scrollbar_calculate_thumb(
        model,
        track_length,
        minimum_length,
        &thumb);
    travel = track_length - thumb.length;
    if (travel == 0) {
        return model->history_rows;
    }
    if (thumb_start > travel) {
        thumb_start = travel;
    }
    return scale_rounded(
        thumb_start,
        model->history_rows,
        travel);
}
