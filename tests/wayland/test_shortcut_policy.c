/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "shortcut_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

int main(void)
{
    boxrig_key_event event;

    memset(&event, 0, sizeof(event));
    event.codepoint = '=';
    event.shifted_codepoint = '+';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_term_wayland_shortcut_command_match(&event, true) ==
          BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_IN);

    event.codepoint = ',';
    event.shifted_codepoint = '<';
    CHECK(boxrig_term_wayland_shortcut_command_match(&event, true) ==
          BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_DECREASE);
    CHECK(boxrig_term_wayland_shortcut_command_match(&event, false) ==
          BOXRIG_TERM_WAYLAND_SHORTCUT_NONE);

    event.codepoint = 'c';
    event.shifted_codepoint = 'C';
    CHECK(boxrig_term_wayland_shortcut_command_match(&event, true) ==
          BOXRIG_TERM_WAYLAND_SHORTCUT_COPY);

    printf("PASS native Wayland sample application shortcut policy\n");
    return 0;
}
