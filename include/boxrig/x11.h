/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_H
#define BOXRIG_X11_H

/**
 * @file
 * Public X11 component, renderer, input, host, and dispatch interfaces.
 *
 * @remarks Native Xlib types are intentionally confined to this platform
 * header and never appear in the platform-independent boxrig UI headers.
 */

#include "boxrig/x11/event.h"
#include "boxrig/x11/component.h"
#include "boxrig/x11/host.h"
#include "boxrig/x11/input.h"
#include "boxrig/x11/renderer.h"
#include "boxrig/x11/scrollbar.h"
#include "boxrig/x11/transparency.h"
#include "boxrig/x11/wait.h"

#endif
