/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_GL_SURFACE_H
#define BOXRIG_GTK_GL_SURFACE_H

#include "boxrig/gtk/rendering.h"
#include "boxrig/render/opengl.h"

/* Retains terminal pixels independently of GTK's rotating presentation target.
 * The caller keeps the borrowed context current for creation, drawing,
 * presentation and ordinary destruction. No native window or swap is owned. */
typedef struct boxrig_gtk_gl_surface boxrig_gtk_gl_surface;

boxrig_ui_status boxrig_gtk_gl_surface_create(
    const boxrig_ogl_renderer_config *config, boxrig_gtk_gl_surface **surface_out,
    boxrig_gtk_render_state *state);

void boxrig_gtk_gl_surface_destroy(boxrig_gtk_gl_surface **surface);

/* Dirty remains unaccepted until the complete image has been transferred. */
boxrig_ui_status boxrig_gtk_gl_surface_draw(boxrig_gtk_gl_surface *surface,
    boxrig_ui_view *view, boxrig_composition *composition,
    boxrig_gtk_render_state *state);

boxrig_ui_status boxrig_gtk_gl_surface_present(boxrig_gtk_gl_surface *surface,
    const boxrig_gl_target *destination, boxrig_gtk_render_state *state);

boxrig_ui_status boxrig_gtk_gl_surface_get_target(
    const boxrig_gtk_gl_surface *surface, boxrig_gl_target *target_out);

unsigned int boxrig_gtk_gl_surface_cell_height(const boxrig_gtk_gl_surface *surface);

int boxrig_gtk_gl_surface_get_statistics(const boxrig_gtk_gl_surface *surface,
    boxrig_ogl_render_statistics *statistics_out);

void boxrig_gtk_gl_surface_fail_next(boxrig_gtk_gl_surface *surface,
    boxrig_ogl_failure_point point);

#endif
