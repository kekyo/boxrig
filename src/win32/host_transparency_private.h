/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_HOST_TRANSPARENCY_PRIVATE_H
#define BOXRIG_WIN32_HOST_TRANSPARENCY_PRIVATE_H
#include "boxrig/win32/host_transparency.h"

boxrig_ui_status boxrig_win32_transparency_host_begin_native(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_action *action);
void boxrig_win32_transparency_host_end_native(boxrig_win32_transparency_host *host);
#endif
