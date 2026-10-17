/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_GL_AREA_H
#define BOXRIG_GTK_GL_AREA_H

#include "boxrig/gtk/rendering.h"
#include "boxrig/render/opengl.h"
#include "gl_destination.h"
#include <gtk/gtk.h>

/* Private, GTK-version-neutral borrowed-context bridge. Compile with one GTK. */
GtkWidget *boxrig_gtk_gl_area_new(void);

boxrig_gl_function boxrig_gtk_gl_area_resolve(void *context, const char *name);

/* Called from render, after GTK has attached its actual draw framebuffer. */
boxrig_ui_status boxrig_gtk_gl_area_describe(
    GtkGLArea *area, uint64_t context_generation, uint64_t target_generation,
    boxrig_gl_runtime_config *runtime, boxrig_gtk_gl_destination *destination,
    boxrig_gtk_render_state *state);

void boxrig_gtk_gl_area_record_error(boxrig_gtk_render_state *state,
    boxrig_gtk_render_reason reason, boxrig_ui_status status, const char *message);

void boxrig_gtk_gl_area_record_diagnostic(boxrig_gtk_render_state *state,
    const boxrig_gl_diagnostic *diagnostic);

#endif
