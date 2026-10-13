/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "shortcut_policy.h"

#include "boxrig/ui/shortcut.h"

boxrig_term_wayland_shortcut_command
boxrig_term_wayland_shortcut_command_match(
    const boxrig_key_event *event,
    bool enable_background_opacity)
{
    boxrig_ui_clipboard_shortcut clipboard;
    boxrig_ui_background_opacity_shortcut opacity;
    boxrig_ui_zoom_shortcut zoom;

    clipboard = boxrig_ui_clipboard_shortcut_match(event);
    if (clipboard == BOXRIG_UI_CLIPBOARD_SHORTCUT_COPY) {
        return BOXRIG_TERM_WAYLAND_SHORTCUT_COPY;
    }
    if (clipboard == BOXRIG_UI_CLIPBOARD_SHORTCUT_PASTE) {
        return BOXRIG_TERM_WAYLAND_SHORTCUT_PASTE;
    }
    opacity = boxrig_ui_background_opacity_shortcut_match(event);
    if (enable_background_opacity &&
        opacity != BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE) {
        if (opacity == BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE) {
            return BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_DECREASE;
        }
        if (opacity == BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE) {
            return BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_INCREASE;
        }
        return BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_TOGGLE;
    }
    zoom = boxrig_ui_zoom_shortcut_match(event);
    if (zoom == BOXRIG_UI_ZOOM_SHORTCUT_IN) {
        return BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_IN;
    }
    if (zoom == BOXRIG_UI_ZOOM_SHORTCUT_OUT) {
        return BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_OUT;
    }
    if (zoom == BOXRIG_UI_ZOOM_SHORTCUT_RESET) {
        return BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_RESET;
    }
    return BOXRIG_TERM_WAYLAND_SHORTCUT_NONE;
}
