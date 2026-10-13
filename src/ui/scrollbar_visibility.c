/* Shared standard-scrollbar layout transaction - Under MIT. */
#include "scrollbar_visibility.h"

void boxrig_ui_scrollbar_visibility_initialize(
    boxrig_ui_scrollbar_visibility_controller *controller,
    bool supported, unsigned int width)
{
    *controller = (boxrig_ui_scrollbar_visibility_controller){0};
    controller->state.visible = supported;
    controller->state.reserved_width = supported ? width : 0;
    controller->target_visible = supported;
    controller->target_width = controller->state.reserved_width;
}

boxrig_ui_status boxrig_ui_scrollbar_visibility_prepare(
    boxrig_ui_scrollbar_visibility_controller *controller, boxrig_ui_view *view,
    boxrig_ui_scrollbar_visibility policy, bool supported, unsigned int width,
    const boxrig_ui_allocation_input *full_allocation)
{
    if (policy < BOXRIG_UI_SCROLLBAR_ALWAYS || policy > BOXRIG_UI_SCROLLBAR_NEVER)
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (controller->state.pending) return BOXRIG_UI_BUSY;
    boxrig_scrollbar_state scrollbar;
    boxrig_ui_status status = boxrig_ui_view_get_scrollbar_state(view, &scrollbar);
    if (status != BOXRIG_UI_OK) return status;
    bool visible = supported && (policy == BOXRIG_UI_SCROLLBAR_ALWAYS ||
        (policy == BOXRIG_UI_SCROLLBAR_AUTO && scrollbar.model.enabled));
    unsigned int target_width = visible ? width : 0;
    if (visible == controller->state.visible && target_width == controller->state.reserved_width) {
        controller->state.requested = policy;
        return BOXRIG_UI_OK;
    }
    boxrig_ui_size_command_state command;
    boxrig_ui_layout_action action;
    if (boxrig_ui_view_get_size_command_state(view, &command) != BOXRIG_UI_OK ||
        command != BOXRIG_UI_SIZE_COMMAND_IDLE ||
        boxrig_ui_view_peek_layout_action(view, &action)) return BOXRIG_UI_BUSY;
    if (full_allocation != NULL) {
        boxrig_ui_allocation_input input = *full_allocation;
        boxrig_ui_fixed reserved = (boxrig_ui_fixed)target_width * BOXRIG_UI_FIXED_ONE;
        input.allocation_width = input.allocation_width > reserved
            ? input.allocation_width - reserved : BOXRIG_UI_FIXED_ONE;
        boxrig_ui_layout_result layout;
        status = boxrig_ui_view_accept_allocation(view, &input, &layout);
        if (status != BOXRIG_UI_OK) return status;
    }
    controller->previous_policy = controller->state.requested;
    controller->state.requested = policy;
    controller->target_visible = visible;
    controller->target_width = target_width;
    controller->state.pending = true;
    return BOXRIG_UI_OK;
}

void boxrig_ui_scrollbar_visibility_commit(
    boxrig_ui_scrollbar_visibility_controller *controller)
{
    controller->state.visible = controller->target_visible;
    controller->state.reserved_width = controller->target_width;
    controller->state.pending = false;
}

void boxrig_ui_scrollbar_visibility_cancel(
    boxrig_ui_scrollbar_visibility_controller *controller)
{
    if (!controller->state.pending) return;
    controller->state.requested = controller->previous_policy;
    controller->target_visible = controller->state.visible;
    controller->target_width = controller->state.reserved_width;
    controller->state.pending = false;
}
