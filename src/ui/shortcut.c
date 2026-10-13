/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/shortcut.h"
#include "boxrig/ui/pixel.h"

boxrig_ui_zoom_shortcut boxrig_ui_zoom_shortcut_match(
    const boxrig_key_event *event)
{
    const uint32_t shortcut_modifiers =
        BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
        BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SUPER |
        BOXRIG_MODIFIER_HYPER | BOXRIG_MODIFIER_META;
    uint32_t modifiers;
    uint32_t symbol;

    if (event == NULL || event->action != BOXRIG_KEY_PRESS) {
        return BOXRIG_UI_ZOOM_SHORTCUT_NONE;
    }
    modifiers = event->modifiers & shortcut_modifiers;
    if ((modifiers & BOXRIG_MODIFIER_CONTROL) == 0 ||
        (modifiers & (BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_SUPER |
                      BOXRIG_MODIFIER_HYPER |
                      BOXRIG_MODIFIER_META)) != 0) {
        return BOXRIG_UI_ZOOM_SHORTCUT_NONE;
    }
    if (event->key == BOXRIG_KEY_KP_ADD) {
        return BOXRIG_UI_ZOOM_SHORTCUT_IN;
    }
    if (event->key == BOXRIG_KEY_KP_SUBTRACT) {
        return BOXRIG_UI_ZOOM_SHORTCUT_OUT;
    }
    if (event->key == BOXRIG_KEY_KP_0) {
        return BOXRIG_UI_ZOOM_SHORTCUT_RESET;
    }
    if (event->key != BOXRIG_KEY_NONE) {
        return BOXRIG_UI_ZOOM_SHORTCUT_NONE;
    }
    symbol = (modifiers & BOXRIG_MODIFIER_SHIFT) != 0 &&
                     event->shifted_codepoint != 0
                 ? event->shifted_codepoint
                 : event->codepoint;
    if (symbol == '+') {
        return BOXRIG_UI_ZOOM_SHORTCUT_IN;
    }
    if (symbol == '-') {
        return BOXRIG_UI_ZOOM_SHORTCUT_OUT;
    }
    if (symbol == '0') {
        return BOXRIG_UI_ZOOM_SHORTCUT_RESET;
    }
    return BOXRIG_UI_ZOOM_SHORTCUT_NONE;
}

boxrig_ui_background_opacity_shortcut
boxrig_ui_background_opacity_shortcut_match(
    const boxrig_key_event *event)
{
    const uint32_t shortcut_modifiers =
        BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
        BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SUPER |
        BOXRIG_MODIFIER_HYPER | BOXRIG_MODIFIER_META;
    uint32_t modifiers;
    uint32_t symbol;

    if (event == NULL || event->action != BOXRIG_KEY_PRESS) {
        return BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE;
    }
    modifiers = event->modifiers & shortcut_modifiers;
    if ((modifiers & BOXRIG_MODIFIER_CONTROL) == 0 ||
        (modifiers & (BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_SUPER |
                      BOXRIG_MODIFIER_HYPER |
                      BOXRIG_MODIFIER_META)) != 0 ||
        event->key != BOXRIG_KEY_NONE) {
        return BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE;
    }
    symbol = (modifiers & BOXRIG_MODIFIER_SHIFT) != 0 &&
                     event->shifted_codepoint != 0
                 ? event->shifted_codepoint
                 : event->codepoint;
    if (symbol == '<') {
        return BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE;
    }
    if (symbol == '>') {
        return BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE;
    }
    if (symbol == 'O') {
        return BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE;
    }
    return BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE;
}

boxrig_ui_clipboard_shortcut boxrig_ui_clipboard_shortcut_match(
    const boxrig_key_event *event)
{
    const uint32_t shortcut_modifiers =
        BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
        BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SUPER |
        BOXRIG_MODIFIER_HYPER | BOXRIG_MODIFIER_META;
    uint32_t modifiers;

    if (event == NULL || event->action != BOXRIG_KEY_PRESS) {
        return BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE;
    }
    modifiers = event->modifiers & shortcut_modifiers;
    if ((modifiers & (BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_SUPER |
                      BOXRIG_MODIFIER_HYPER |
                      BOXRIG_MODIFIER_META)) != 0) {
        return BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE;
    }
    if (event->key == BOXRIG_KEY_INSERT) {
        return modifiers == BOXRIG_MODIFIER_SHIFT
                   ? BOXRIG_UI_CLIPBOARD_SHORTCUT_PASTE
                   : BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE;
    }
    if (event->key != BOXRIG_KEY_NONE ||
        modifiers !=
            (BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT)) {
        return BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE;
    }
    if (event->shifted_codepoint == 'C') {
        return BOXRIG_UI_CLIPBOARD_SHORTCUT_COPY;
    }
    if (event->shifted_codepoint == 'V') {
        return BOXRIG_UI_CLIPBOARD_SHORTCUT_PASTE;
    }
    return BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE;
}

static uint32_t background_opacity_percent(uint8_t opacity)
{
    return ((uint32_t)opacity * 100u + 127u) / 255u;
}

bool boxrig_ui_background_opacity_shortcut_next(
    boxrig_ui_background_opacity_shortcut shortcut,
    uint8_t current_opacity,
    uint8_t remembered_opacity,
    uint8_t *next_opacity_out,
    uint8_t *next_remembered_opacity_out)
{
    uint32_t current_percent;
    uint32_t remembered_percent;
    uint32_t next_percent;
    uint32_t next_remembered_percent;
    uint8_t next_opacity;
    uint8_t next_remembered_opacity;

    if (next_opacity_out == NULL || next_remembered_opacity_out == NULL) {
        return false;
    }
    current_percent = background_opacity_percent(current_opacity);
    remembered_percent = background_opacity_percent(remembered_opacity);
    if (remembered_percent == 100) {
        remembered_percent = 80;
    }
    next_percent = current_percent;
    next_remembered_percent = remembered_percent;
    switch (shortcut) {
    case BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE:
        if (current_percent == 0) {
            return false;
        }
        next_percent = current_percent > 10 ? current_percent - 10 : 0;
        next_remembered_percent = next_percent;
        break;
    case BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE:
        if (current_percent == 100) {
            return false;
        }
        next_percent = current_percent < 90 ? current_percent + 10 : 100;
        if (next_percent < 100) {
            next_remembered_percent = next_percent;
        }
        break;
    case BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE:
        if (current_percent == 100) {
            next_percent = remembered_percent;
            next_remembered_percent = remembered_percent;
        } else {
            next_percent = 100;
            next_remembered_percent = current_percent;
        }
        break;
    case BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE:
    default:
        return false;
    }
    if (!boxrig_ui_background_opacity_from_percent(
            next_percent,
            &next_opacity) ||
        !boxrig_ui_background_opacity_from_percent(
            next_remembered_percent,
            &next_remembered_opacity) ||
        next_opacity == current_opacity) {
        return false;
    }
    *next_opacity_out = next_opacity;
    *next_remembered_opacity_out = next_remembered_opacity;
    return true;
}
