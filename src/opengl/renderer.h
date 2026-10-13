/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_OPENGL_RENDERER_PRIVATE_REDIRECT_H
#define BOXRIG_OPENGL_RENDERER_PRIVATE_REDIRECT_H

#include "../../include/boxrig/render/opengl.h"

#include "text_provider.h"

/* Takes provider ownership only on success; used by native component adapters. */
boxrig_gl_status boxrig_ogl_renderer_create_with_provider(
    const boxrig_ogl_renderer_config *, boxrig_ogl_text_provider *,
    boxrig_ogl_renderer **, boxrig_gl_diagnostic *);

#endif
