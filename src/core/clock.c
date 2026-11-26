/* boxrig - Under MIT. */
#include "terminal_internal.h"
#include <stdlib.h>

boxrig_status boxrig_terminal_update_monotonic_time(
    boxrig_terminal *terminal, uint64_t milliseconds)
{
    if (terminal != NULL && terminal->animation_clock != NULL) {
        return BOXRIG_STATUS_BUSY;
    }
    return boxrig_update_monotonic_time(terminal, milliseconds);
}

boxrig_status boxrig_terminal_acquire_animation_clock(
    boxrig_terminal *terminal, boxrig_animation_clock **clock_out)
{
    if (clock_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *clock_out = NULL;
    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (terminal->animation_clock != NULL) {
        return BOXRIG_STATUS_BUSY;
    }
    boxrig_animation_clock *clock = calloc(1, sizeof(*clock));
    if (clock == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    clock->terminal = terminal;
    terminal->animation_clock = clock;
    *clock_out = clock;
    return BOXRIG_STATUS_OK;
}

void boxrig_animation_clock_release(boxrig_animation_clock *clock)
{
    if (clock == NULL) {
        return;
    }
    if (clock->terminal != NULL) {
        clock->terminal->animation_clock = NULL;
    }
    free(clock);
}

boxrig_status boxrig_animation_clock_update_monotonic_time(
    boxrig_animation_clock *clock, uint64_t milliseconds)
{
    if (clock == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (clock->terminal == NULL) {
        return BOXRIG_STATUS_STALE;
    }
    return boxrig_update_monotonic_time(clock->terminal, milliseconds);
}
