/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_H
#define BOXRIG_WIN32_H

/**
 * @file
 * Public Win32 component, renderer, input, host, dispatch, and ConPTY interfaces.
 *
 * @remarks Native Windows types are intentionally confined to this platform
 * header and never appear in the platform-independent boxrig UI headers.
 */

#include "boxrig/win32/component.h"
#include "boxrig/win32/event.h"
#include "boxrig/win32/host.h"
#include "boxrig/win32/host_transparency.h"
#include "boxrig/win32/opacity_native.h"
#include "boxrig/win32/input.h"
#include "boxrig/win32/pty.h"
#include "boxrig/win32/renderer.h"
#include "boxrig/win32/session.h"
#include "boxrig/win32/transparency.h"

#endif
