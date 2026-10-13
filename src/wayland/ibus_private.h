/* boxrig - Under MIT. */
#ifndef BOXRIG_WAYLAND_IBUS_PRIVATE_H
#define BOXRIG_WAYLAND_IBUS_PRIVATE_H

#include "input_private.h"

bool boxrig_wayland_ibus_initialize(boxrig_wayland_context *context);
void boxrig_wayland_ibus_destroy(boxrig_wayland_context *context);
bool boxrig_wayland_ibus_selected(const boxrig_wayland_context *context);
uint32_t boxrig_wayland_ibus_modifiers(boxrig_wayland_seat *seat,
    xkb_keysym_t symbol, bool release);
void boxrig_wayland_ibus_focus(boxrig_wayland_seat *seat,
    boxrig_wayland_terminal_component *component, bool commit);
void boxrig_wayland_ibus_remove_seat(boxrig_wayland_seat *seat);
void boxrig_wayland_ibus_forget_component(boxrig_wayland_terminal_component *component);
void boxrig_wayland_ibus_update(boxrig_wayland_context *context);
void boxrig_wayland_ibus_cursor(boxrig_wayland_terminal_component *component, bool placed);
void boxrig_wayland_ibus_diagnostic(const boxrig_wayland_context *context,
    boxrig_wayland_context_diagnostic *diagnostic);
boxrig_ui_status boxrig_wayland_ibus_filter_key(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item, bool *blocked, bool *handled);
boxrig_ui_status boxrig_wayland_ibus_apply_reply(boxrig_wayland_terminal_component *component,
    boxrig_wayland_input_item *item, bool *blocked, bool *consumed);
void boxrig_wayland_ibus_release_request(struct boxrig_ibus_request *request);

#endif
