/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_LINK_GESTURE_H
#define BOXRIG_TERM_LINK_GESTURE_H

#include "text_policy.h"

/** One pending application link gesture owned by a sample view. */
typedef struct boxrig_term_link_gesture boxrig_term_link_gesture;

/**
 * Starts one external request; reports failure without retrying the gesture.
 * @param context Borrowed launcher state.
 * @param uri Validated, complete UTF-8 HTTP/HTTPS URI.
 * @param length URI bytes excluding its NUL terminator.
 * @remarks The URI is borrowed only for this call. Copy it before retaining it.
 * Do not destroy or reenter the gesture from this callback.
 */
typedef void (*boxrig_term_link_open)(void *context, const char *uri, size_t length);

/**
 * Creates a gesture whose terminal, view, policy and callback context outlive it.
 * @param terminal Source terminal.
 * @param view View used for authoritative hits and selection.
 * @param policy Existing URL/OSC policy owned by this view.
 * @param open Callback that starts an external request after confirmation.
 * @param context Borrowed callback state.
 * @param gesture_out Receives the owned gesture, cleared on failure.
 * @return Zero, or -1 with errno for invalid arguments/allocation failure.
 */
int boxrig_term_link_gesture_create(
    boxrig_terminal *terminal, boxrig_ui_view *view,
    boxrig_term_text_policy *policy, boxrig_term_link_open open, void *context,
    boxrig_term_link_gesture **gesture_out);

/**
 * Cancels both a captured press and a released click awaiting its deadline.
 * @param gesture Gesture to cancel, or NULL.
 * @remarks Call on leave, focus/capture loss, hide and before replacing a view.
 */
void boxrig_term_link_gesture_cancel(boxrig_term_link_gesture *gesture);

/**
 * Cancels and frees the gesture without starting an external request.
 * @param gesture Gesture to destroy, or NULL.
 */
void boxrig_term_link_gesture_destroy(boxrig_term_link_gesture *gesture);

/**
 * Routes link clicks before the existing copy/wheel/selection policy.
 * @param gesture Live view gesture.
 * @param selection_drag_active Existing selection capture, set on drag transfer.
 * @param event Current logical event.
 * @param hit Authoritative hit belonging to event.
 * @param now Absolute host monotonic milliseconds.
 * @param multi_click_interval Native multi-click interval, in milliseconds.
 * @param multi_click_distance Native multi-click distance in view fixed units.
 * @param drag_distance Nonnegative native drag threshold in view fixed units.
 * @return APPLICATION while the link owns a series, TERMINAL to continue the
 * existing application/terminal routing, or ERROR for invalid arguments/OOM.
 * @remarks Lock modifiers are ignored. Other modifiers and terminal reporting
 * prevent launch. A drag transfers its original anchor to ordinary selection.
 * A fresh single press outside native multi-click bounds can confirm a
 * previously released click immediately. A zero native interval is accepted.
 */
boxrig_input_mouse_route boxrig_term_link_gesture_route(
    boxrig_term_link_gesture *gesture, bool *selection_drag_active,
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    uint64_t now, uint32_t multi_click_interval,
    boxrig_ui_fixed multi_click_distance, boxrig_ui_fixed drag_distance);

/**
 * Reads the one-shot deadline after a qualifying release.
 * @param gesture Gesture to inspect.
 * @param deadline_out Receives absolute milliseconds only when true is returned.
 * @return Whether a deadline is pending; false for NULL arguments.
 */
bool boxrig_term_link_gesture_deadline(
    const boxrig_term_link_gesture *gesture, uint64_t *deadline_out);

/**
 * Revalidates and confirms a due click once, without changing selection/hover.
 * @param gesture Live gesture.
 * @param now Absolute milliseconds in the route clock domain.
 * @return Zero, or -1 with errno on a query failure; failures cancel the click.
 * @remarks Early notifications do nothing. The host rebooks any remaining
 * deadline. A stale or replaced link is silently canceled. External request
 * errors are the callback's responsibility and never trigger automatic retry.
 */
int boxrig_term_link_gesture_dispatch(boxrig_term_link_gesture *gesture, uint64_t now);

#endif
