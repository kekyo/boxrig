/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_GL_DESTINATION_H
#define BOXRIG_GTK_GL_DESTINATION_H

#include <stdbool.h>
#include <stdint.h>

/* Actual borrowed GTK color attachment. This is a transfer destination, never
 * a public renderer target: the renderer always owns a separate RGBA image. */
typedef struct boxrig_gtk_gl_destination {
    uint32_t draw_framebuffer;
    uint32_t width;
    uint32_t height;
    uint64_t generation;
    bool srgb;
    unsigned int alpha_bits;
    unsigned int samples;
} boxrig_gtk_gl_destination;

#endif
