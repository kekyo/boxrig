/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/lifecycle.h"

#include <stdio.h>

int main(void)
{
    boxrig_ui_lifecycle lifecycle;

    boxrig_ui_lifecycle_initialize(&lifecycle);
    if (boxrig_ui_lifecycle_get_state(&lifecycle) !=
            BOXRIG_UI_LIFECYCLE_RUNNING ||
        !boxrig_ui_lifecycle_accepts_events(&lifecycle) ||
        boxrig_ui_lifecycle_mark_detached(&lifecycle) != -1 ||
        boxrig_ui_lifecycle_mark_stopped(&lifecycle) != -1 ||
        boxrig_ui_lifecycle_get_state(&lifecycle) !=
            BOXRIG_UI_LIFECYCLE_RUNNING) {
        fprintf(stderr, "lifecycle accepted a skipped shutdown phase\n");
        return 1;
    }
    if (boxrig_ui_lifecycle_begin_shutdown(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_accepts_events(&lifecycle) ||
        boxrig_ui_lifecycle_get_state(&lifecycle) !=
            BOXRIG_UI_LIFECYCLE_QUIESCING ||
        boxrig_ui_lifecycle_begin_shutdown(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_mark_detached(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_get_state(&lifecycle) !=
            BOXRIG_UI_LIFECYCLE_DETACHED ||
        boxrig_ui_lifecycle_mark_detached(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_mark_stopped(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_get_state(&lifecycle) !=
            BOXRIG_UI_LIFECYCLE_STOPPED ||
        boxrig_ui_lifecycle_begin_shutdown(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_mark_detached(&lifecycle) == -1 ||
        boxrig_ui_lifecycle_mark_stopped(&lifecycle) == -1) {
        fprintf(stderr, "lifecycle did not preserve ordered idempotent shutdown\n");
        return 1;
    }
    printf("PASS UI lifecycle suppresses events and orders shutdown\n");
    return 0;
}
