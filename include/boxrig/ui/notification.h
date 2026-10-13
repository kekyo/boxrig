/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_NOTIFICATION_H
#define BOXRIG_UI_NOTIFICATION_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Monotonic millisecond clock used by notification policy. */
typedef uint64_t (*boxrig_notification_policy_clock)(void *context);

/** Configuration for untrusted terminal notification requests. */
typedef struct boxrig_notification_policy_config {
    /** Whether any terminal notification requests may be displayed. */
    bool allow_notifications;
    /** Maximum and initial number of display tokens. */
    size_t token_capacity;
    /** Milliseconds required to replenish one display token. */
    uint64_t token_refill_milliseconds;
    /** Milliseconds for which a successful identical request is a duplicate. */
    uint64_t duplicate_milliseconds;
    /** Maximum successful identifiers retained for duplicate detection. */
    size_t tracking_capacity;
    /** Monotonic clock function. */
    boxrig_notification_policy_clock clock;
    /** Opaque value passed to clock. */
    void *clock_context;
} boxrig_notification_policy_config;

/** Opaque mutable notification policy state. */
typedef struct boxrig_notification_policy_state
    boxrig_notification_policy_state;

/** Result of evaluating one notification request. */
typedef enum boxrig_notification_policy_decision {
    /** Reject the request because policy or rate limits disallow it. */
    BOXRIG_NOTIFICATION_POLICY_DENIED = 0,
    /** Complete successfully without another provider call. */
    BOXRIG_NOTIFICATION_POLICY_DUPLICATE,
    /** Invoke the desktop notification provider. */
    BOXRIG_NOTIFICATION_POLICY_DISPLAY,
    /** Complete unavailable because the provider cannot be used. */
    BOXRIG_NOTIFICATION_POLICY_UNAVAILABLE
} boxrig_notification_policy_decision;

/** One deterministic notification policy evaluation. */
typedef struct boxrig_notification_policy_evaluation {
    /** Policy decision. */
    boxrig_notification_policy_decision decision;
    /** Monotonic timestamp used for duplicate and token decisions. */
    uint64_t timestamp_milliseconds;
} boxrig_notification_policy_evaluation;

/**
 * Returns the sample notification policy defaults.
 *
 * @param clock Monotonic millisecond clock used by the policy.
 * @param clock_context Opaque value passed to clock.
 * @return Default-deny policy with five tokens, a two-second refill, a
 * one-second duplicate interval, and 64 tracked identifiers.
 */
boxrig_notification_policy_config boxrig_notification_policy_config_default(
    boxrig_notification_policy_clock clock,
    void *clock_context);

/**
 * Creates mutable policy state.
 *
 * @param config Valid policy configuration copied by the state.
 * @param state_out Receives the new state.
 * @return Zero on success, or -1 for invalid arguments or allocation failure.
 */
int boxrig_notification_policy_create(
    const boxrig_notification_policy_config *config,
    boxrig_notification_policy_state **state_out);

/**
 * Evaluates a notification without calling a desktop provider.
 *
 * @param state Policy state.
 * @param notification Valid normalized notification view.
 * @param window_focused Whether the terminal window has keyboard focus.
 * @param window_visible Whether the terminal window is visible to the user.
 * @param provider_available Whether the provider can currently be called.
 * @return Decision and the monotonic timestamp used for it.
 *
 * @remarks A DISPLAY decision consumes one token. DUPLICATE and UNAVAILABLE
 * decisions do not. The caller must invoke the provider exactly once for a
 * DISPLAY decision and record successful calls with
 * boxrig_notification_policy_record_success().
 */
boxrig_notification_policy_evaluation boxrig_notification_policy_evaluate(
    boxrig_notification_policy_state *state,
    const boxrig_notification_view *notification,
    bool window_focused,
    bool window_visible,
    bool provider_available);

/**
 * Records a successful provider display or update for duplicate detection.
 *
 * @param state Policy state.
 * @param notification Notification passed to the successful provider call.
 * @param timestamp_milliseconds Timestamp returned by its evaluation.
 * @return Zero on success, or -1 for invalid data or allocation failure.
 *
 * @remarks Anonymous notifications are intentionally not retained.
 */
int boxrig_notification_policy_record_success(
    boxrig_notification_policy_state *state,
    const boxrig_notification_view *notification,
    uint64_t timestamp_milliseconds);

/**
 * Releases policy state and all retained notification content.
 *
 * @param state State to destroy, or NULL.
 */
void boxrig_notification_policy_destroy(
    boxrig_notification_policy_state *state);

#endif
