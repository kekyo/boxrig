/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/input.h"

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
