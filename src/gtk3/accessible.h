/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK3_ACCESSIBLE_H
#define BOXRIG_GTK3_ACCESSIBLE_H

#include "boxrig/gtk3/terminal_widget.h"

GType boxrig_gtk3_terminal_accessible_get_type(void);
void boxrig_gtk3_accessible_set_busy(BoxrigGtk3TerminalWidget *widget, bool busy);
boxrig_ui_status boxrig_gtk3_accessible_update(BoxrigGtk3TerminalWidget *widget);
void boxrig_gtk3_accessible_detach(BoxrigGtk3TerminalWidget *widget);

#endif
