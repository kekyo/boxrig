/* boxrig - Under MIT. */
#ifndef BOXRIG_WIN32_ANIMATION_H
#define BOXRIG_WIN32_ANIMATION_H

#include "boxrig/ui/animation.h"

typedef struct boxrig_win32_animation boxrig_win32_animation;

/* Acquires the calling UI thread's shared message-loop scheduler. */
boxrig_ui_status boxrig_win32_animation_acquire(
    boxrig_win32_animation **animation_out);

/* All attached views must be detached before releasing their references. */
void boxrig_win32_animation_release(boxrig_win32_animation *animation);

boxrig_ui_animation_runtime *boxrig_win32_animation_runtime(
    boxrig_win32_animation *animation);

#endif
