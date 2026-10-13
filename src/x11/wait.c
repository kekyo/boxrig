/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/wait.h"

#include <errno.h>

int boxrig_x11_wait_for_activity(
    Display *display,
    struct pollfd *descriptors,
    nfds_t descriptor_count,
    int timeout)
{
    int result;

    if (display == NULL || descriptors == NULL || descriptor_count == 0 ||
        timeout < -1) {
        errno = EINVAL;
        return -1;
    }
    if (XPending(display) > 0) {
        return 1;
    }
    do {
        result = poll(descriptors, descriptor_count, timeout);
    } while (result == -1 && errno == EINTR);
    return result;
}
