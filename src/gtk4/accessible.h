/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK4_ACCESSIBLE_H
#define BOXRIG_GTK4_ACCESSIBLE_H

#include "boxrig/gtk4/terminal_widget.h"
#include "../gtk/accessible_text.h"

/* Private frontend state, owned and detached by the terminal widget. */
boxrig_gtk_accessible_state *boxrig_gtk4_accessible_state(BoxrigGtk4TerminalWidget *widget);
void boxrig_gtk4_accessible_text_init(GtkAccessibleTextInterface *interface);
boxrig_ui_status boxrig_gtk4_accessible_update(BoxrigGtk4TerminalWidget *widget);

#endif
