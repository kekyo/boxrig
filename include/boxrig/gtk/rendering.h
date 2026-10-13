/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_GTK_RENDERING_H
#define BOXRIG_GTK_RENDERING_H

#include "boxrig/ui/view.h"

/** Requested terminal rendering policy, independent of GTK's compositor. */
typedef enum boxrig_gtk_render_policy {
    /** Default: use Cairo without attempting a boxrig GL context. */
    BOXRIG_GTK_RENDER_CAIRO = 0,
    /** Try OpenGL or OpenGL ES; fall back to Cairo after a detected GL failure. */
    BOXRIG_GTK_RENDER_OPENGL_PREFERRED,
    /** Require OpenGL or OpenGL ES; report failure without silently using Cairo. */
    BOXRIG_GTK_RENDER_OPENGL_REQUIRED
} boxrig_gtk_render_policy;

/** Backend with a successfully completed terminal image. */
typedef enum boxrig_gtk_render_backend {
    /** No backend has completed an image yet. */
    BOXRIG_GTK_RENDER_BACKEND_NONE = 0,
    /** Pango/Cairo software terminal rendering. */
    BOXRIG_GTK_RENDER_BACKEND_CAIRO,
    /** Borrowed-context OpenGL or OpenGL ES terminal rendering. */
    BOXRIG_GTK_RENDER_BACKEND_OPENGL
} boxrig_gtk_render_backend;

/** Reason for the most recent rendering selection or failure. */
typedef enum boxrig_gtk_render_reason {
    /** No rendering failure has been recorded. */
    BOXRIG_GTK_RENDER_REASON_NONE = 0,
    /** The caller explicitly selected a rendering policy. */
    BOXRIG_GTK_RENDER_REASON_REQUESTED,
    /** This boxrig GTK library was built without OpenGL support. */
    BOXRIG_GTK_RENDER_REASON_NOT_BUILT,
    /** GTK could not create or make its GL context current. */
    BOXRIG_GTK_RENDER_REASON_CONTEXT,
    /** The GL API, version, profile, function set, or target is unsupported. */
    BOXRIG_GTK_RENDER_REASON_CAPABILITY,
    /** Memory, shader, or framebuffer resources could not be created. */
    BOXRIG_GTK_RENDER_REASON_RESOURCE,
    /** Drawing or presenting an image failed. */
    BOXRIG_GTK_RENDER_REASON_DRAW,
    /** The previously used context or target was lost. */
    BOXRIG_GTK_RENDER_REASON_CONTEXT_LOST
} boxrig_gtk_render_reason;

/** Bounded rendering diagnostics without GL headers or allocation ownership. */
typedef struct boxrig_gtk_render_state {
    /** Most recently requested policy; default is Cairo. */
    boxrig_gtk_render_policy requested_policy;
    /** Backend of the last successfully completed presentation. */
    boxrig_gtk_render_backend active_backend;
    /** A requested backend or fallback is waiting for its first complete image. */
    bool pending;
    /** Reason for the last selection or failure. */
    boxrig_gtk_render_reason reason;
    /** Last selection/GL operation result, retained after successful fallback. */
    boxrig_ui_status status;
    /** GL error code when one was reported, otherwise zero. */
    uint32_t gl_code;
    /** Graphics reset status when available, otherwise zero. */
    uint32_t reset_status;
    /** Generation of the current GTK context, or zero before GL initialization. */
    uint64_t context_generation;
    /** Generation of the current terminal framebuffer, or zero before creation. */
    uint64_t target_generation;
    /** Last validated GL context uses OpenGL ES; false before GL initialization. */
    bool is_gles;
    /** NUL-terminated explanation; remains valid after a creation failure. */
    char message[512];
} boxrig_gtk_render_state;

#endif
