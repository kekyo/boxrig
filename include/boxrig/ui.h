/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_H
#define BOXRIG_UI_H

/* This umbrella is deliberately platform-neutral. Native frontend headers live
 * under boxrig/x11.h and boxrig/win32.h and are never included here. */
#include "boxrig/ui/animation.h"
#include "boxrig/ui/animation_poll.h"
#include "boxrig/ui/blink.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/color.h"
#include "boxrig/ui/component.h"
#include "boxrig/ui/editable.h"
#include "boxrig/ui/image.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/input_text.h"
#include "boxrig/ui/lifecycle.h"
#include "boxrig/ui/notification.h"
#include "boxrig/ui/notification_dispatch.h"
#include "boxrig/ui/pixel.h"
#include "boxrig/ui/scrollbar.h"
#include "boxrig/ui/shortcut.h"
#include "boxrig/ui/text_layout.h"
#include "boxrig/ui/view.h"

#endif
