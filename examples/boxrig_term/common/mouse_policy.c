/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "mouse_policy.h"

static boxrig_ui_selection_kind selection_kind_for_click_count(
    uint8_t click_count)
{
    if (click_count >= 3u) {
        return BOXRIG_UI_SELECTION_LINE;
    }
    if (click_count == 2u) {
        return BOXRIG_UI_SELECTION_WORD;
    }
    return BOXRIG_UI_SELECTION_CHARACTER;
}

boxrig_input_mouse_route boxrig_term_mouse_policy_route_selection(
    boxrig_terminal *terminal,
    boxrig_ui_view *view,
    bool *selection_drag_active,
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    bool *selection_completed_out)
{
    boxrig_ui_selection_options options;
    boxrig_ui_status status;
    bool completing;
    bool host_override;

    if (terminal == NULL || view == NULL || selection_drag_active == NULL ||
        event == NULL || hit == NULL || selection_completed_out == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    *selection_completed_out = false;
    completing = event->action == BOXRIG_MOUSE_RELEASE &&
                 event->button == BOXRIG_MOUSE_BUTTON_LEFT;
    if (*selection_drag_active) {
        if ((event->action == BOXRIG_MOUSE_MOTION || completing) &&
            hit->inside_grid && hit->text_position_valid) {
            status = boxrig_ui_view_update_selection(view, hit);
            if (status != BOXRIG_UI_OK) {
                if (completing) {
                    *selection_drag_active = false;
                }
                return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
            }
        }
        if (completing) {
            boxrig_ui_selection selection;
            *selection_drag_active = false;
            *selection_completed_out = boxrig_ui_view_get_selection(view, &selection) &&
                (selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR ||
                 selection.row_count != 0);
        }
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if (event->action != BOXRIG_MOUSE_PRESS ||
        event->button != BOXRIG_MOUSE_BUTTON_LEFT) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    host_override =
        (event->modifiers &
         (BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL)) != 0u;
    if (boxrig_terminal_mouse_reporting_enabled(terminal) &&
        !host_override) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    if (!hit->inside_grid || !hit->text_position_valid) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    options.shape =
        (event->modifiers & BOXRIG_MODIFIER_CONTROL) != 0u
            ? BOXRIG_UI_SELECTION_RECTANGULAR
            : BOXRIG_UI_SELECTION_LINEAR;
    options.kind = options.shape == BOXRIG_UI_SELECTION_RECTANGULAR
                       ? BOXRIG_UI_SELECTION_CHARACTER
                       : selection_kind_for_click_count(event->click_count);
    status = boxrig_ui_view_begin_selection_with_options(view, &options, hit);
    if (status != BOXRIG_UI_OK) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    *selection_drag_active = true;
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}

boxrig_input_mouse_route boxrig_term_mouse_policy_route_wheel(
    boxrig_terminal *terminal, bool selection_drag_active,
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    boxrig_ui_zoom_shortcut *zoom_out)
{
    if (terminal == NULL || event == NULL || hit == NULL || zoom_out == NULL)
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    *zoom_out = BOXRIG_UI_ZOOM_SHORTCUT_NONE;
    if (selection_drag_active || !hit->inside_grid ||
        event->action != BOXRIG_MOUSE_PRESS ||
        (event->button != BOXRIG_MOUSE_BUTTON_WHEEL_UP &&
         event->button != BOXRIG_MOUSE_BUTTON_WHEEL_DOWN))
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    uint32_t modifiers = event->modifiers &
        ~(uint32_t)(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK);
    if ((modifiers & ~(uint32_t)(BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT)) != 0)
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    bool up = event->button == BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    if ((modifiers & BOXRIG_MODIFIER_CONTROL) != 0) {
        *zoom_out = up ? BOXRIG_UI_ZOOM_SHORTCUT_IN : BOXRIG_UI_ZOOM_SHORTCUT_OUT;
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if ((modifiers & BOXRIG_MODIFIER_SHIFT) == 0 &&
        (boxrig_terminal_mouse_reporting_enabled(terminal) ||
         (boxrig_terminal_alternate_scroll_enabled(terminal) &&
          boxrig_terminal_active_text_screen(terminal) == BOXRIG_TEXT_SCREEN_ALTERNATE)))
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    boxrig_scrollbar_model model;
    if (boxrig_scrollbar_model_read(terminal, &model) == -1)
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    if (!model.enabled) return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    /* Compute a single clamped transaction in oldest-to-live coordinates. */
    size_t position = up ? (model.position < 3 ? 0 : model.position - 3)
        : (model.history_rows - model.position < 3 ? model.history_rows : model.position + 3);
    bool changed;
    if (boxrig_scrollbar_apply_action(terminal, BOXRIG_SCROLLBAR_POSITION,
            position, &changed) == -1)
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}

boxrig_input_mouse_route boxrig_term_mouse_policy_route_copy(
    boxrig_ui_view *view, bool selection_drag_active, bool *copy_button_active,
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    bool *copy_out, boxrig_ui_status *status_out)
{
    if (view == NULL || copy_button_active == NULL || event == NULL ||
        hit == NULL || copy_out == NULL || status_out == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    *copy_out = false;
    *status_out = BOXRIG_UI_OK;
    if (*copy_button_active) {
        if (event->button == BOXRIG_MOUSE_BUTTON_RIGHT) {
            if (event->action == BOXRIG_MOUSE_RELEASE) {
                *copy_button_active = false;
            }
            return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
        }
        if (event->action == BOXRIG_MOUSE_MOTION) {
            if ((event->pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_RIGHT) != 0) {
                return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
            }
            /* A native capture loss may omit the release transition. */
            *copy_button_active = false;
        }
    }
    if (selection_drag_active || event->action != BOXRIG_MOUSE_PRESS ||
        event->button != BOXRIG_MOUSE_BUTTON_RIGHT || !hit->inside_grid) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_ui_status status = boxrig_ui_view_create_selection_snapshot(
        view, &snapshot);
    if (status == BOXRIG_UI_UNAVAILABLE) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    if (status == BOXRIG_UI_OK) {
        size_t length;
        (void)boxrig_text_snapshot_data(snapshot, &length);
        boxrig_text_snapshot_destroy(snapshot);
        if (length == 0) {
            return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
        }
    }
    *copy_button_active = true;
    *copy_out = status == BOXRIG_UI_OK;
    *status_out = status;
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
}
