/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/lifecycle.h"

#include <stddef.h>

static int advance_lifecycle(
    boxrig_ui_lifecycle *lifecycle,
    boxrig_ui_lifecycle_state required,
    boxrig_ui_lifecycle_state next)
{
    if (lifecycle == NULL) {
        return -1;
    }
    if (lifecycle->state >= next &&
        lifecycle->state <= BOXRIG_UI_LIFECYCLE_STOPPED) {
        return 0;
    }
    if (lifecycle->state != required) {
        return -1;
    }
    lifecycle->state = next;
    return 0;
}

void boxrig_ui_lifecycle_initialize(boxrig_ui_lifecycle *lifecycle)
{
    if (lifecycle != NULL) {
        lifecycle->state = BOXRIG_UI_LIFECYCLE_RUNNING;
    }
}

boxrig_ui_lifecycle_state boxrig_ui_lifecycle_get_state(
    const boxrig_ui_lifecycle *lifecycle)
{
    return lifecycle == NULL
               ? BOXRIG_UI_LIFECYCLE_STOPPED
               : lifecycle->state;
}

bool boxrig_ui_lifecycle_accepts_events(
    const boxrig_ui_lifecycle *lifecycle)
{
    return lifecycle != NULL &&
           lifecycle->state == BOXRIG_UI_LIFECYCLE_RUNNING;
}

int boxrig_ui_lifecycle_begin_shutdown(boxrig_ui_lifecycle *lifecycle)
{
    return advance_lifecycle(
        lifecycle,
        BOXRIG_UI_LIFECYCLE_RUNNING,
        BOXRIG_UI_LIFECYCLE_QUIESCING);
}

int boxrig_ui_lifecycle_mark_detached(boxrig_ui_lifecycle *lifecycle)
{
    return advance_lifecycle(
        lifecycle,
        BOXRIG_UI_LIFECYCLE_QUIESCING,
        BOXRIG_UI_LIFECYCLE_DETACHED);
}

int boxrig_ui_lifecycle_mark_stopped(boxrig_ui_lifecycle *lifecycle)
{
    return advance_lifecycle(
        lifecycle,
        BOXRIG_UI_LIFECYCLE_DETACHED,
        BOXRIG_UI_LIFECYCLE_STOPPED);
}
