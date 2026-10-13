/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/shortcut.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/pixel.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool test_zoom_shortcuts_use_logical_key_identity(void)
{
    boxrig_key_event event;

    memset(&event, 0, sizeof(event));
    event.codepoint = '=';
    event.shifted_codepoint = '+';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_IN);

    event.codepoint = '-';
    event.shifted_codepoint = '_';
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_OUT);

    event.codepoint = '0';
    event.shifted_codepoint = ')';
    event.action = BOXRIG_KEY_RELEASE;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_CAPS_LOCK;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    event.action = BOXRIG_KEY_PRESS;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_RESET);

    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_KP_ADD;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_NUM_LOCK;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_IN);
    event.key = BOXRIG_KEY_KP_SUBTRACT;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_OUT);
    event.key = BOXRIG_KEY_KP_0;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_RESET);
    event.action = BOXRIG_KEY_REPEAT;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    event.action = BOXRIG_KEY_RELEASE;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    return true;
}

static bool test_zoom_shortcuts_reject_unmodified_and_ambiguous_keys(void)
{
    boxrig_key_event event;

    memset(&event, 0, sizeof(event));
    event.codepoint = '=';
    event.shifted_codepoint = '+';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);

    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_ALT |
                      BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);

    event.codepoint = '-';
    event.shifted_codepoint = '_';
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_zoom_shortcut_match(&event) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    CHECK(boxrig_ui_zoom_shortcut_match(NULL) ==
          BOXRIG_UI_ZOOM_SHORTCUT_NONE);
    return true;
}

static bool test_background_opacity_shortcuts_use_logical_symbols(void)
{
    boxrig_key_event event;

    memset(&event, 0, sizeof(event));
    event.codepoint = ',';
    event.shifted_codepoint = '<';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE);

    event.codepoint = '.';
    event.shifted_codepoint = '>';
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE);

    event.codepoint = 'o';
    event.shifted_codepoint = 'O';
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE);

    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE);
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT |
                      BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE);
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    event.action = BOXRIG_KEY_REPEAT;
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE);
    event.action = BOXRIG_KEY_RELEASE;
    CHECK(boxrig_ui_background_opacity_shortcut_match(&event) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE);
    CHECK(boxrig_ui_background_opacity_shortcut_match(NULL) ==
          BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE);
    return true;
}

static bool test_background_opacity_shortcuts_step_and_toggle_percentages(void)
{
    uint8_t opacity;
    uint8_t remembered;
    uint8_t next;
    uint8_t next_remembered;

    CHECK(boxrig_ui_background_opacity_from_percent(100, &opacity));
    remembered = opacity;
    CHECK(boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE,
        opacity,
        remembered,
        &next,
        &next_remembered));
    CHECK(next == 230);
    CHECK(next_remembered == 230);

    opacity = next;
    remembered = next_remembered;
    CHECK(boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE,
        opacity,
        remembered,
        &next,
        &next_remembered));
    CHECK(next == 255);
    CHECK(next_remembered == 230);

    opacity = next;
    remembered = next_remembered;
    CHECK(boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE,
        opacity,
        remembered,
        &next,
        &next_remembered));
    CHECK(next == 230);
    CHECK(next_remembered == 230);
    CHECK(boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE,
        next,
        next_remembered,
        &opacity,
        &remembered));
    CHECK(opacity == 255);
    CHECK(remembered == 230);

    CHECK(boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE,
        UINT8_MAX,
        UINT8_MAX,
        &next,
        &next_remembered));
    CHECK(next == 204);
    CHECK(next_remembered == 204);

    CHECK(boxrig_ui_background_opacity_from_percent(5, &opacity));
    CHECK(boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE,
        opacity,
        opacity,
        &next,
        &next_remembered));
    CHECK(next == 0);
    CHECK(next_remembered == 0);
    CHECK(!boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE,
        next,
        next_remembered,
        &opacity,
        &remembered));
    CHECK(!boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_NONE,
        next,
        next_remembered,
        &opacity,
        &remembered));
    CHECK(!boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE,
        UINT8_MAX,
        next_remembered,
        &opacity,
        &remembered));
    CHECK(!boxrig_ui_background_opacity_shortcut_next(
        BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE,
        UINT8_MAX,
        next_remembered,
        NULL,
        &remembered));
    return true;
}

static bool test_clipboard_shortcuts_use_portable_logical_keys(void)
{
    boxrig_key_event event;

    memset(&event, 0, sizeof(event));
    event.codepoint = 'c';
    event.shifted_codepoint = 'C';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_COPY);

    event.codepoint = 'v';
    event.shifted_codepoint = 'V';
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_PASTE);

    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_INSERT;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_PASTE);

    event.action = BOXRIG_KEY_REPEAT;
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE);
    event.action = BOXRIG_KEY_RELEASE;
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE);
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE);
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_ui_clipboard_shortcut_match(&event) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE);
    CHECK(boxrig_ui_clipboard_shortcut_match(NULL) ==
          BOXRIG_UI_CLIPBOARD_SHORTCUT_NONE);
    return true;
}

static bool test_portable_view_actions_check_target_capabilities(void)
{
    boxrig_ui_view_action_capabilities capabilities;
    boxrig_ui_view_action action;

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.copy_targets =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    capabilities.paste_targets = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;

    action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
    action.clipboard_target =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
    CHECK(boxrig_ui_view_action_supported(&capabilities, &action));
    action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(!boxrig_ui_view_action_supported(&capabilities, &action));

    action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
    CHECK(boxrig_ui_view_action_supported(&capabilities, &action));
    action.clipboard_target =
        BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION |
        BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
    CHECK(!boxrig_ui_view_action_supported(&capabilities, &action));
    CHECK(!boxrig_ui_view_action_supported(NULL, &action));
    CHECK(!boxrig_ui_view_action_supported(&capabilities, NULL));
    return true;
}

int main(void)
{
    if (!test_zoom_shortcuts_use_logical_key_identity() ||
        !test_zoom_shortcuts_reject_unmodified_and_ambiguous_keys() ||
        !test_background_opacity_shortcuts_use_logical_symbols() ||
        !test_background_opacity_shortcuts_step_and_toggle_percentages() ||
        !test_clipboard_shortcuts_use_portable_logical_keys() ||
        !test_portable_view_actions_check_target_capabilities()) {
        return 1;
    }
    printf("PASS portable example shortcut matching and opacity policy\n");
    return 0;
}
