/* Application-owned scrollbar binding example - Under MIT.
 * Keep this binding for the component lifetime. Initialize component, leaving
 * revision and updating zero. Call refresh from the scheduled frame after
 * component_frame(); the frame-request callback only schedules that frame.
 * set_bar updates the application's range, page size, position and enabled
 * state. Its synchronous change signal may safely call action below.
 * The application owns the bar and its geometry; set scrollbar_presentation
 * to BOXRIG_UI_COMPONENT_SCROLLBAR_NONE in the component configuration.
 */
#ifndef EXAMPLE_WAYLAND_EXTERNAL_SCROLLBAR_H
#define EXAMPLE_WAYLAND_EXTERNAL_SCROLLBAR_H
#include "boxrig/wayland.h"

typedef struct example_external_scrollbar {
    boxrig_wayland_terminal_component *component;
    uint64_t revision;
    bool updating;
} example_external_scrollbar;

static inline boxrig_ui_status example_external_scrollbar_action(
    example_external_scrollbar *bar, boxrig_scrollbar_action action,
    size_t position, bool *changed_out)
{
    if (bar->updating) {
        *changed_out = false;
        return BOXRIG_UI_OK;
    }
    return boxrig_wayland_terminal_component_apply_scrollbar_action(
        bar->component, action, position, changed_out);
}

static inline boxrig_ui_status example_external_scrollbar_refresh(
    example_external_scrollbar *bar,
    void (*set_bar)(void *, const boxrig_scrollbar_state *), void *context)
{
    boxrig_scrollbar_state state;
    boxrig_ui_status status = boxrig_wayland_terminal_component_get_scrollbar_state(
        bar->component, &state);
    if (status != BOXRIG_UI_OK || bar->updating ||
        bar->revision == state.revision) return status;
    bar->revision = state.revision;
    bar->updating = true;
    set_bar(context, &state);
    bar->updating = false;
    return BOXRIG_UI_OK;
}

/* For a custom-drawn thumb, calculate geometry from state.model with
 * boxrig_scrollbar_calculate_thumb(). Convert a drag using
 * boxrig_scrollbar_position_from_thumb() and send SCROLLBAR_POSITION.
 * Native controls can send row positions directly. No polling is needed. */
#endif
