/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_IME_PROBE_PRIVATE_H
#define BOXRIG_GTK_IME_PROBE_PRIVATE_H
#include "boxrig/gtk/input.h"
#include <gtk/gtk.h>

/* The caller owns the subscription. Only GTK dispatches its borrowed Wayland
 * connection; the probe never binds an input object or reads the display. */
typedef struct boxrig_gtk_ime_probe boxrig_gtk_ime_probe;
boxrig_gtk_ime_probe *boxrig_gtk_ime_probe_new(GdkDisplay *display,
    GtkIMContext *context, boxrig_gtk_ime_policy policy, boxrig_ui_candidate_policy candidates,
    void (*changed)(void *), void *data);
void boxrig_gtk_ime_probe_destroy(boxrig_gtk_ime_probe *probe);
/* Returns false while the initial registry sync is outstanding. */
bool boxrig_gtk_ime_probe_get(boxrig_gtk_ime_probe *probe,
    boxrig_gtk_ime_state *state, bool *direct);
#endif
