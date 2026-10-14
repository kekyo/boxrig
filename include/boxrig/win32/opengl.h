/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_OPENGL_H
#define BOXRIG_WIN32_OPENGL_H

#include "boxrig/render/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed WGL context/thread binding, without window or context ownership. */
typedef struct boxrig_wgl_binding boxrig_wgl_binding;

/**
 * Captures the caller's current WGL context and thread for one generation.
 * @param context_generation Nonzero caller-assigned context generation.
 * @param target_generation Nonzero initial framebuffer generation.
 * @param binding_out Receives the binding owned by the caller.
 * @param runtime_out Receives callbacks borrowed for the binding lifetime.
 * @param diagnostic_out Optional bounded diagnostic.
 * @return OpenGL status; context and thread changes are rejected by callbacks.
 * @remarks Destroy all renderers before releasing the binding. Recreated
 * contexts require a new binding and a new, increasing context generation.
 */
boxrig_gl_status boxrig_wgl_binding_create(
    uint64_t context_generation,
    uint64_t target_generation,
    boxrig_wgl_binding **binding_out,
    boxrig_gl_runtime_config *runtime_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Releases only the binding, without accessing the caller's context or FBO.
 * @param binding Binding to release; NULL is accepted.
 */
void boxrig_wgl_binding_destroy(boxrig_wgl_binding *binding);

#ifdef __cplusplus
}
#endif

#endif
