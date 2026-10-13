/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "root_backing_policy.h"

#include <stdint.h>

boxrig_term_wayland_root_candidate_status
boxrig_term_wayland_root_choose_candidate(
    const boxrig_term_wayland_root_slot *slots,
    size_t slot_count,
    size_t current_index,
    size_t maximum_bytes,
    size_t candidate_bytes,
    boxrig_term_wayland_root_candidate *candidate_out)
{
    size_t candidate_index = slot_count;
    size_t retained_bytes = 0;
    size_t index;

    if (slots == NULL ||
        slot_count != BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT ||
        current_index > slot_count || maximum_bytes == 0 ||
        maximum_bytes > BOXRIG_TERM_WAYLAND_ROOT_BYTES_MAXIMUM ||
        candidate_bytes == 0 || candidate_out == NULL) {
        return BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID;
    }
    for (index = 0; index < slot_count; index++) {
        if (slots[index].allocated) {
            if (retained_bytes > SIZE_MAX - slots[index].bytes) {
                return BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID;
            }
            retained_bytes += slots[index].bytes;
        } else if (slots[index].bytes != 0 || slots[index].released) {
            return BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID;
        }
        if (index != current_index &&
            (!slots[index].allocated || slots[index].released) &&
            candidate_index == slot_count) {
            candidate_index = index;
        }
    }
    if (candidate_index == slot_count) {
        return BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_BUSY;
    }
    if (slots[candidate_index].allocated) {
        retained_bytes -= slots[candidate_index].bytes;
    }
    if (candidate_bytes > maximum_bytes ||
        retained_bytes > maximum_bytes - candidate_bytes) {
        return BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_LIMIT;
    }
    candidate_out->index = candidate_index;
    candidate_out->recycle_released = slots[candidate_index].allocated;
    return BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_OK;
}

boxrig_term_wayland_root_update_action
boxrig_term_wayland_root_candidate_action(
    boxrig_term_wayland_root_candidate_status status)
{
    switch (status) {
    case BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_OK:
        return BOXRIG_TERM_WAYLAND_ROOT_UPDATE_APPLY;
    case BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_BUSY:
        return BOXRIG_TERM_WAYLAND_ROOT_UPDATE_RETRY;
    case BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_LIMIT:
        return BOXRIG_TERM_WAYLAND_ROOT_UPDATE_REJECT;
    case BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID:
    default:
        return BOXRIG_TERM_WAYLAND_ROOT_UPDATE_FAIL;
    }
}

uint32_t boxrig_term_wayland_root_clear_pixel(bool argb)
{
    return argb ? UINT32_C(0) : UINT32_C(0xff202430);
}
