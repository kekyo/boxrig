/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_WAIT_H
#define BOXRIG_X11_WAIT_H

#include <X11/Xlib.h>

#include <poll.h>

/**
 * Waits for activity in the Xlib event queue or the supplied descriptors.
 *
 * @param display X11 connection whose client-side event queue is inspected.
 * @param descriptors Descriptors passed to poll().
 * @param descriptor_count Number of entries in descriptors.
 * @param timeout Poll timeout in milliseconds, or -1 to wait indefinitely.
 * @return A positive value when activity is available, zero on timeout, or -1
 * with errno set on failure.
 */
int boxrig_x11_wait_for_activity(
    Display *display,
    struct pollfd *descriptors,
    nfds_t descriptor_count,
    int timeout);

#endif
