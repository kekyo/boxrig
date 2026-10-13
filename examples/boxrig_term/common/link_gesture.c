/* boxrig - Under MIT. */
#include "link_gesture.h"
#include "mouse_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct boxrig_term_link_gesture {
    boxrig_terminal *terminal;
    boxrig_ui_view *view;
    boxrig_term_text_policy *policy;
    boxrig_term_link_open open;
    void *context;
    boxrig_term_link_target target;
    boxrig_text_range anchor;
    boxrig_ui_fixed press_x;
    boxrig_ui_fixed press_y;
    boxrig_ui_fixed release_x;
    boxrig_ui_fixed release_y;
    boxrig_ui_fixed drag_distance;
    uint64_t deadline;
    bool pressed;
    bool released;
    bool consume_release;
};

int boxrig_term_link_gesture_create(
    boxrig_terminal *terminal, boxrig_ui_view *view,
    boxrig_term_text_policy *policy, boxrig_term_link_open open, void *context,
    boxrig_term_link_gesture **gesture_out)
{
    if (gesture_out == NULL) { errno = EINVAL; return -1; }
    *gesture_out = NULL;
    if (terminal == NULL || view == NULL || policy == NULL || open == NULL) {
        errno = EINVAL;
        return -1;
    }
    boxrig_term_link_gesture *gesture = calloc(1, sizeof(*gesture));
    if (gesture == NULL) { errno = ENOMEM; return -1; }
    gesture->terminal = terminal;
    gesture->view = view;
    gesture->policy = policy;
    gesture->open = open;
    gesture->context = context;
    *gesture_out = gesture;
    return 0;
}

void boxrig_term_link_gesture_cancel(boxrig_term_link_gesture *gesture)
{
    if (gesture == NULL) return;
    boxrig_term_link_target_clear(&gesture->target);
    gesture->pressed = false;
    gesture->released = false;
    gesture->consume_release = false;
}

void boxrig_term_link_gesture_destroy(boxrig_term_link_gesture *gesture)
{
    boxrig_term_link_gesture_cancel(gesture);
    free(gesture);
}

static bool same_position(const boxrig_text_position *a, const boxrig_text_position *b)
{
    return a->screen == b->screen && a->line_id.epoch == b->line_id.epoch &&
        a->line_id.serial == b->line_id.serial && a->column == b->column &&
        a->line_revision == b->line_revision;
}

static bool same_target(boxrig_term_link_gesture *gesture, const boxrig_term_link_target *current)
{
    const boxrig_term_link_target *original = &gesture->target;
    if (original->kind == BOXRIG_TERM_TEXT_POINTER_NONE ||
        original->kind != current->kind || original->uri_length != current->uri_length ||
        current->uri == NULL || memcmp(original->uri, current->uri, original->uri_length) != 0)
        return false;
    boxrig_text_range rebased;
    if (boxrig_terminal_rebase_text_range(gesture->terminal, &original->range, &rebased) != BOXRIG_STATUS_OK)
        return false;
    if (original->kind == BOXRIG_TERM_TEXT_POINTER_OSC8)
        return original->hyperlink_handle == current->hyperlink_handle;
    return original->source_id == current->source_id && original->item_id == current->item_id &&
        same_position(&rebased.start, &current->range.start) &&
        same_position(&rebased.end, &current->range.end);
}

static int confirm(boxrig_term_link_gesture *gesture)
{
    if (!gesture->released) return 0;
    if (boxrig_terminal_mouse_reporting_enabled(gesture->terminal)) {
        boxrig_term_link_gesture_cancel(gesture);
        return 0;
    }
    boxrig_ui_hit_test_result hit;
    boxrig_term_link_target current = {0};
    if (boxrig_ui_view_hit_test(gesture->view, gesture->release_x,
            gesture->release_y, &hit) != BOXRIG_UI_OK ||
        boxrig_term_text_policy_query_link(gesture->policy, &hit, &current) == -1) {
        boxrig_term_link_gesture_cancel(gesture);
        return -1;
    }
    bool valid = same_target(gesture, &current);
    boxrig_term_link_target_clear(&current);
    // Remove the reservation before the external request, including failures.
    gesture->released = false;
    if (valid) gesture->open(gesture->context, gesture->target.uri, gesture->target.uri_length);
    boxrig_term_link_gesture_cancel(gesture);
    return 0;
}

bool boxrig_term_link_gesture_deadline(const boxrig_term_link_gesture *gesture, uint64_t *deadline_out)
{
    if (gesture == NULL || deadline_out == NULL || !gesture->released) return false;
    *deadline_out = gesture->deadline;
    return true;
}

int boxrig_term_link_gesture_dispatch(boxrig_term_link_gesture *gesture, uint64_t now)
{
    if (gesture == NULL) { errno = EINVAL; return -1; }
    return gesture->released && now >= gesture->deadline ? confirm(gesture) : 0;
}

static bool unmodified(const boxrig_mouse_event *event)
{
    return (event->modifiers & ~(uint32_t)(BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK)) == 0;
}

static uint64_t distance(boxrig_ui_fixed a, boxrig_ui_fixed b)
{
    // Unsigned subtraction also covers opposite signed extremes without UB.
    return a >= b ? (uint64_t)a - (uint64_t)b : (uint64_t)b - (uint64_t)a;
}

static boxrig_input_mouse_route begin_drag(
    boxrig_term_link_gesture *gesture, bool *selection_drag_active, bool releasing)
{
    boxrig_ui_hit_test_result anchor;
    boxrig_text_range current;
    bool valid = boxrig_terminal_rebase_text_range(gesture->terminal,
        &gesture->anchor, &current) == BOXRIG_STATUS_OK &&
        boxrig_ui_view_hit_test(gesture->view, gesture->press_x,
            gesture->press_y, &anchor) == BOXRIG_UI_OK &&
        anchor.inside_grid && anchor.text_position_valid &&
        same_position(&current.start, &anchor.leading_position) &&
        same_position(&current.end, &anchor.trailing_position);
    boxrig_term_link_gesture_cancel(gesture);
    if (!valid) {
        gesture->consume_release = !releasing;
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    boxrig_mouse_event press = {0};
    press.action = BOXRIG_MOUSE_PRESS;
    press.button = BOXRIG_MOUSE_BUTTON_LEFT;
    press.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    press.click_count = 1;
    // Selection owns the series after transfer, even if reporting changed.
    press.modifiers = BOXRIG_MODIFIER_SHIFT;
    bool completed;
    boxrig_input_mouse_route route = boxrig_term_mouse_policy_route_selection(
        gesture->terminal, gesture->view, selection_drag_active, &press, &anchor, &completed);
    return route == BOXRIG_INPUT_MOUSE_ROUTE_ERROR ? route : BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

boxrig_input_mouse_route boxrig_term_link_gesture_route(
    boxrig_term_link_gesture *gesture, bool *selection_drag_active,
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    uint64_t now, uint32_t interval, boxrig_ui_fixed multi_click_distance,
    boxrig_ui_fixed drag_distance)
{
    if (gesture == NULL || selection_drag_active == NULL || event == NULL ||
        hit == NULL || multi_click_distance < 0 || drag_distance < 0) {
        errno = EINVAL;
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (*selection_drag_active) {
        boxrig_term_link_gesture_cancel(gesture);
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    if (event->action == BOXRIG_MOUSE_PRESS) {
        bool candidate = event->button == BOXRIG_MOUSE_BUTTON_LEFT &&
            event->click_count == 1 && unmodified(event) && hit->inside_grid &&
            hit->text_position_valid && !boxrig_terminal_mouse_reporting_enabled(gesture->terminal);
        /* GDK 3 sends a plain press immediately before the promoted double
         * or triple press. Preserve native multi-click precedence until its
         * time/position bounds exclude that promotion. */
        bool may_promote = gesture->released && now < gesture->deadline &&
            distance(hit->x, gesture->press_x) <= (uint64_t)multi_click_distance &&
            distance(hit->y, gesture->press_y) <= (uint64_t)multi_click_distance;
        if (candidate && gesture->released && !may_promote && confirm(gesture) == -1)
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        boxrig_term_link_gesture_cancel(gesture);
        if (!candidate) return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
        boxrig_term_link_target target = {0};
        if (boxrig_term_text_policy_query_link(gesture->policy, hit, &target) == -1)
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        if (target.kind == BOXRIG_TERM_TEXT_POINTER_NONE) return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
        uint64_t delay = (uint64_t)interval + 1;
        if (now > UINT64_MAX - delay) {
            boxrig_term_link_target_clear(&target);
            errno = EOVERFLOW;
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        gesture->target = target;
        gesture->anchor.start = hit->leading_position;
        gesture->anchor.end = hit->trailing_position;
        gesture->press_x = hit->x;
        gesture->press_y = hit->y;
        gesture->drag_distance = drag_distance;
        gesture->deadline = now + delay;
        gesture->pressed = true;
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if (gesture->consume_release) {
        if ((event->action == BOXRIG_MOUSE_RELEASE && event->button == BOXRIG_MOUSE_BUTTON_LEFT) ||
            (event->action == BOXRIG_MOUSE_MOTION && (event->pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_LEFT) == 0))
            boxrig_term_link_gesture_cancel(gesture);
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if (!gesture->pressed) return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    bool releasing = event->action == BOXRIG_MOUSE_RELEASE && event->button == BOXRIG_MOUSE_BUTTON_LEFT;
    if (event->action == BOXRIG_MOUSE_MOTION &&
        (event->pressed_buttons & BOXRIG_MOUSE_BUTTON_MASK_LEFT) == 0) {
        boxrig_term_link_gesture_cancel(gesture);
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    if (event->action != BOXRIG_MOUSE_MOTION && !releasing)
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    if (distance(hit->x, gesture->press_x) > (uint64_t)gesture->drag_distance ||
        distance(hit->y, gesture->press_y) > (uint64_t)gesture->drag_distance)
        return begin_drag(gesture, selection_drag_active, releasing);
    if (!releasing) return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    gesture->pressed = false;
    if (!unmodified(event) || boxrig_terminal_mouse_reporting_enabled(gesture->terminal)) {
        boxrig_term_link_gesture_cancel(gesture);
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    boxrig_term_link_target current = {0};
    if (boxrig_term_text_policy_query_link(gesture->policy, hit, &current) == -1) {
        boxrig_term_link_gesture_cancel(gesture);
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    bool valid = same_target(gesture, &current);
    boxrig_term_link_target_clear(&current);
    if (!valid) {
        boxrig_term_link_gesture_cancel(gesture);
        return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
    gesture->release_x = hit->x;
    gesture->release_y = hit->y;
    gesture->released = true;
    return boxrig_term_link_gesture_dispatch(gesture, now) == 0
        ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
}
