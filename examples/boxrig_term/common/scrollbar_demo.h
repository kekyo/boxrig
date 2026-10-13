/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_SCROLLBAR_DEMO_H
#define BOXRIG_TERM_SCROLLBAR_DEMO_H

#include "boxrig/ui/component.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/view.h"

/** Borrowed native visibility setter, invoked outside input dispatch. */
typedef boxrig_ui_status (*boxrig_term_scrollbar_demo_setter)(
    void *context, boxrig_ui_scrollbar_visibility policy);

/** Optional driver control; all referenced objects outlive this state. */
typedef struct boxrig_term_scrollbar_demo {
    boxrig_ui_view *view; /**< Borrowed view used to request dispatch. */
    boxrig_term_scrollbar_demo_setter setter; /**< Native policy setter. */
    void *context; /**< Borrowed setter context. */
    boxrig_ui_scrollbar_visibility policy; /**< Latest requested policy. */
    bool enabled; /**< BOXRIG_SCROLLBAR explicitly enables the control. */
    bool pending; /**< Setter must retry after an existing layout completes. */
} boxrig_term_scrollbar_demo;

/**
 * Reads BOXRIG_SCROLLBAR=always|auto|never and applies its initial policy.
 * @param demo Caller-owned demonstration state.
 * @param view Borrowed terminal view.
 * @param setter Native visibility setter.
 * @param context Borrowed setter context.
 * @return Zero on success (including deferred work), -1 on invalid configuration.
 * @remarks An unset variable preserves the frontend default and disables F9.
 */
int boxrig_term_scrollbar_demo_initialize(boxrig_term_scrollbar_demo *demo,
    boxrig_ui_view *view, boxrig_term_scrollbar_demo_setter setter, void *context);

/**
 * Applies the latest pending policy outside a component frame.
 * @param demo Initialized demonstration state.
 * @return Zero on success or native BUSY, -1 on a native failure.
 */
int boxrig_term_scrollbar_demo_apply(boxrig_term_scrollbar_demo *demo);

/**
 * Queues always, auto, never in sequence on Ctrl+Shift+F9.
 * @param demo Initialized demonstration state.
 * @param event Normalized key event; repeats and releases do not advance.
 * @return Application ownership when enabled and matched, terminal otherwise.
 */
boxrig_input_key_route boxrig_term_scrollbar_demo_route_key(
    boxrig_term_scrollbar_demo *demo, const boxrig_key_event *event);

#endif
