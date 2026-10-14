/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_WGL_H
#define BOXRIG_WIN32_WGL_H
#include <windows.h>
#include "opengl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque, thread-confined OpenGL ES 3.0 or desktop OpenGL 3.3 core child surface. */
typedef struct boxrig_wgl_surface boxrig_wgl_surface;

/** Fixed-size snapshot of the probed context and selected window pixel format. */
typedef struct boxrig_wgl_surface_info {
    /** Actual context capabilities; vendor text is diagnostic, not a whitelist. */
    boxrig_gl_capabilities gl;
    /** Caller-assigned generation represented by this snapshot. */
    uint64_t context_generation;
    /** Selected window pixel format, set exactly once. */
    int pixel_format;
    /** Whether the selected format provides full ICD acceleration. */
    bool accelerated;
    /** Whether this window's default framebuffer supports sRGB storage. */
    bool srgb;
    /** Whether WGL swap control is advertised and its entry point is present. */
    bool swap_control;
    /** Actual alpha storage bits; zero still permits opaque rendering.
     * This does not establish desktop transparency, which belongs to the host. */
    unsigned int alpha_bits;
} boxrig_wgl_surface_info;

/**
 * Copies creation-time diagnostics without changing the caller's current context.
 * @param surface Surface on its creation thread.
 * @param info_out Receives a bounded snapshot, including driver identification.
 * @return OpenGL status; session type alone never implies GPU support.
 * @remarks Recreate after display/driver changes or resume to obtain a fresh probe.
 */
boxrig_gl_status boxrig_wgl_surface_get_info(
    const boxrig_wgl_surface *surface, boxrig_wgl_surface_info *info_out);

/** Construction values for a double-buffered child surface. */
typedef struct boxrig_wgl_surface_config {
    /** Borrowed parent; must belong to this thread and outlive the surface. */
    HWND parent;
    /** Initial child width in physical pixels, greater than zero. */
    uint32_t width;
    /** Initial child height in physical pixels, greater than zero. */
    uint32_t height;
    /** Nonzero caller-assigned context generation, increased on recreation. */
    uint64_t context_generation;
} boxrig_wgl_surface_config;

/**
 * Creates a hidden candidate child, selecting its pixel format exactly once.
 * @param config Parent, dimensions, and fresh context generation.
 * @param surface_out Receives the owned surface on success. If rollback itself
 * fails, receives retained resources to release with surface_destroy.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return OpenGL status; insufficient extensions, acceleration or version are unavailable.
 * @remarks Tries driver-provided OpenGL ES 3.0, then desktop OpenGL 3.3 core
 * when ES is unsupported or cannot initialize. No EGL or ANGLE library is used.
 * The caller's current context is restored before returning. The child
 * prefers eight alpha bits, falling back to an opaque-capable format. The
 * caller must explicitly prepare its top-level host before presenting alpha;
 * an alpha buffer alone does not provide native window transparency.
 * This adapter owns its DC and context, with no shared context group. All calls
 * must use the creation thread. Destroy renderers before destroying the surface.
 */
boxrig_gl_status boxrig_wgl_surface_create(
    const boxrig_wgl_surface_config *config,
    boxrig_wgl_surface **surface_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Makes this surface current on its creation thread.
 * @param surface Surface to bind.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return OpenGL status; another thread is rejected before any WGL call.
 */
boxrig_gl_status boxrig_wgl_surface_make_current(
    boxrig_wgl_surface *surface, boxrig_gl_diagnostic *diagnostic_out);

/**
 * Releases the current binding on the creation thread.
 * @param surface Surface which must be current.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return OpenGL status. This does not transfer ownership to another thread.
 */
boxrig_gl_status boxrig_wgl_surface_release_current(
    boxrig_wgl_surface *surface, boxrig_gl_diagnostic *diagnostic_out);

/**
 * Copies the renderer-only runtime contract for this surface.
 * @param surface Surface whose context must be current.
 * @param target_generation Nonzero initial caller target generation.
 * @param runtime_out Receives callbacks borrowed for the surface lifetime.
 * @return OpenGL status.
 */
boxrig_gl_status boxrig_wgl_surface_get_runtime(
    boxrig_wgl_surface *surface, uint64_t target_generation,
    boxrig_gl_runtime_config *runtime_out);

/**
 * Returns the owned child handle for sizing and visibility.
 * @param surface Surface to inspect, or NULL.
 * @return Borrowed child HWND, or NULL. Do not destroy it separately.
 */
HWND boxrig_wgl_surface_window(const boxrig_wgl_surface *surface);

/**
 * Reports whether the selected pixel format supports sRGB encoding.
 * @param surface Surface to inspect, or NULL.
 * @return true only if the selected format reports the optional capability.
 */
bool boxrig_wgl_surface_supports_srgb(const boxrig_wgl_surface *surface);

/**
 * Presents the current surface's double buffer.
 * @param surface Surface to present on its creation thread.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return OpenGL status. A failed swap requires target invalidation by the caller.
 */
boxrig_gl_status boxrig_wgl_surface_swap(
    boxrig_wgl_surface *surface, boxrig_gl_diagnostic *diagnostic_out);

/**
 * Releases the context, child (including its private DC), and window class.
 * @param surface Owned pointer, cleared on success; retain and retry on failure.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return OpenGL status. The calling thread must be the creation thread.
 * @remarks Release renderer resources first. A different caller context remains
 * current; if this surface is current, it is unbound before destruction.
 */
boxrig_gl_status boxrig_wgl_surface_destroy(
    boxrig_wgl_surface **surface, boxrig_gl_diagnostic *diagnostic_out);
#ifdef __cplusplus
}
#endif

#endif
