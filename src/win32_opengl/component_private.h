/* Private bridge between the optional DLLs and deterministic component fixtures. */
#ifndef BOXRIG_WGL_COMPONENT_PRIVATE_H
#define BOXRIG_WGL_COMPONENT_PRIVATE_H
#include "../win32/component_private.h"
#include "boxrig/render/opengl.h"
#include "surface_private.h"

boxrig_gl_status boxrig_ogl_renderer_create_with_win32_font(
    const boxrig_ogl_renderer_config *, boxrig_win32_renderer *,
    boxrig_ogl_renderer **, boxrig_gl_diagnostic *);

typedef struct boxrig_win32_ogl_observation {
    HWND window;
    GLuint framebuffer;
    unsigned int width;
    unsigned int height;
    uint64_t context_generation;
    uint64_t target_generation;
    uint64_t swaps;
    bool swap_interval_set;
} boxrig_win32_ogl_observation;

/* Makes the surface current for fixture-only readback. Caller restores its binding. */
bool boxrig_win32_ogl_observe(boxrig_win32_terminal_component *,
    boxrig_win32_ogl_observation *);
#endif
