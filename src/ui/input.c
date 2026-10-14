/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/input.h"
#include "input_control.h"

void boxrig_ui_input_control_initialize(boxrig_ui_input_control *control)
{
    control->state.phase = BOXRIG_UI_INPUT_ENABLED;
    control->state.generation = 1;
    control->paste_end_pending = false;
}

boxrig_ui_status boxrig_ui_input_control_request(
    boxrig_ui_input_control *control, bool enabled, bool paste_started)
{
    if (control->state.phase == BOXRIG_UI_INPUT_STOPPING) {
        return BOXRIG_UI_BUSY;
    }
    if (enabled == (control->state.phase == BOXRIG_UI_INPUT_ENABLED)) {
        return BOXRIG_UI_OK;
    }
    /* Exhaustion must stay closed: a stop is always accepted, but enabling
     * cannot reuse the last generation and admit delayed native input. */
    if (control->state.generation == UINT64_MAX && enabled) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (control->state.generation != UINT64_MAX) {
        control->state.generation++;
    }
    control->paste_end_pending = !enabled && paste_started;
    control->state.phase = enabled ? BOXRIG_UI_INPUT_ENABLED
        : paste_started ? BOXRIG_UI_INPUT_STOPPING : BOXRIG_UI_INPUT_DISABLED;
    return control->paste_end_pending ? BOXRIG_UI_BUSY : BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_input_control_service(
    boxrig_ui_input_control *control, boxrig_terminal *terminal)
{
    if (!control->paste_end_pending) {
        return BOXRIG_UI_OK;
    }
    boxrig_paste_event end = {BOXRIG_PASTE_END, NULL, 0};
    boxrig_status status = boxrig_terminal_send_paste(terminal, &end);
    if (status == BOXRIG_STATUS_WOULD_BLOCK) {
        return BOXRIG_UI_BUSY;
    }
    if (status != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    control->paste_end_pending = false;
    control->state.phase = BOXRIG_UI_INPUT_DISABLED;
    return BOXRIG_UI_OK;
}

bool boxrig_ui_input_control_accepts(
    const boxrig_ui_input_control *control, uint64_t generation)
{
    return control->state.phase == BOXRIG_UI_INPUT_ENABLED &&
           control->state.generation == generation;
}

static bool valid_clipboard_target(uint32_t target)
{
    return target == BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION ||
           target == BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
}

bool boxrig_ui_view_action_supported(
    const boxrig_ui_view_action_capabilities *capabilities,
    const boxrig_ui_view_action *action)
{
    uint32_t supported_targets;

    if (capabilities == NULL || action == NULL) {
        return false;
    }
    if (action->kind == BOXRIG_UI_VIEW_ACTION_RECONVERT) {
        return action->clipboard_target == 0 && capabilities->reconversion;
    }
    if (!valid_clipboard_target(action->clipboard_target)) {
        return false;
    }
    if (action->kind == BOXRIG_UI_VIEW_ACTION_COPY) {
        supported_targets = capabilities->copy_targets;
    } else if (action->kind == BOXRIG_UI_VIEW_ACTION_PASTE) {
        supported_targets = capabilities->paste_targets;
    } else {
        return false;
    }
    return (supported_targets & action->clipboard_target) != 0;
}
