/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_TEXT_POLICY_H
#define BOXRIG_TERM_TEXT_POLICY_H

#include "boxrig/ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Stable annotation source used by the sample's inferred URL rule. */
#define BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID UINT64_C(0x626f787269675552)

/** Default recognizer work assigned to one sample event-loop dispatch. */
#define BOXRIG_TERM_TEXT_POLICY_WORK_BUDGET_DEFAULT ((size_t)4096u)

/** Opaque owner of the sample URL recognizer and its view annotations. */
typedef struct boxrig_term_text_policy boxrig_term_text_policy;

/** Semantic target selected from one pointer-authoritative view hit. */
typedef enum boxrig_term_text_pointer_target {
    /** The pointer does not intersect an application semantic item. */
    BOXRIG_TERM_TEXT_POINTER_NONE = 0,
    /** An explicit OSC 8 hyperlink takes precedence. */
    BOXRIG_TERM_TEXT_POINTER_OSC8,
    /** The pointer intersects the sample's inferred URL annotation. */
    BOXRIG_TERM_TEXT_POINTER_URL,
    /** The pointer intersects only another application annotation. */
    BOXRIG_TERM_TEXT_POINTER_ANNOTATION
} boxrig_term_text_pointer_target;

/**
 * Creates the shared HTTP/HTTPS recognition policy for one sample view.
 *
 * @param terminal Terminal scanned for current and future logical text.
 * @param view View that receives inferred URL underline annotations.
 * @param policy_out Receives the new policy and is cleared on failure.
 * @return Zero on success, or -1 with errno set on failure.
 *
 * @remarks BOXRIG_AUTO_URLS=0 disables inferred URL recognition; 1 or an
 * unset variable enables it. Other values fail with EINVAL. An enabled policy
 * registers one bounded recognizer but performs no scan until
 * boxrig_term_text_policy_resume() is called. Explicit OSC 8 hover remains
 * available when recognition is disabled. The terminal and view must outlive
 * the policy.
 */
int boxrig_term_text_policy_create(
    boxrig_terminal *terminal,
    boxrig_ui_view *view,
    boxrig_term_text_policy **policy_out);

/**
 * Unregisters the URL recognizer, clears its annotations, and frees the policy.
 *
 * @param policy Policy to destroy, or NULL.
 */
void boxrig_term_text_policy_destroy(
    boxrig_term_text_policy *policy);

/**
 * Advances bounded recognition and applies complete line result events.
 *
 * @param policy Policy to advance.
 * @param work_budget Positive core recognizer work limit for this dispatch.
 * @param work_used_out Receives core work no greater than work_budget.
 * @param more_work_out Receives whether another immediate dispatch is needed.
 * @return Zero on success, or -1 with errno set on failure.
 *
 * @remarks Call this after model synchronization and before component frame
 * presentation. At most 64 result events are reflected per call. A lost event
 * or retired annotated line clears the source before a bounded baseline is
 * rebuilt, so no partial annotation set is presented as current. A disabled
 * policy returns zero work and no pending work without advancing recognizers.
 */
int boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out);

/**
 * Updates OSC 8 hover and classifies one exact revisioned pointer hit.
 *
 * @param policy Policy owning the view that produced hit.
 * @param hit Unmodified hit produced from the native event coordinate.
 * @param target_out Receives OSC 8, inferred URL, other annotation, or none.
 * @return Zero on success, or -1 with errno set for an invalid or stale hit.
 *
 * @remarks OSC 8 is selected before inferred URL annotations, which are
 * selected before unrelated annotations regardless of annotation layer
 * priority. This function never opens a URI or starts an external process.
 */
int boxrig_term_text_policy_update_pointer_hover(
    boxrig_term_text_policy *policy,
    const boxrig_ui_hit_test_result *hit,
    boxrig_term_text_pointer_target *target_out);

/**
 * Returns the terminal-lifetime recognizer identity owned by a policy.
 *
 * @param policy Policy to inspect, or NULL.
 * @return Nonzero recognizer identity, or zero for NULL or a disabled policy.
 */
uint64_t boxrig_term_text_policy_recognizer_id(
    const boxrig_term_text_policy *policy);

/** Maximum URI bytes accepted by the sample's HTTP/HTTPS launch policy. */
#define BOXRIG_TERM_LINK_URI_CAPACITY ((size_t)8192u)

/** Owned, validated launch target captured from one authoritative view hit. */
typedef struct boxrig_term_link_target {
    /** OSC 8 or inferred URL, or NONE when no link can be launched. */
    boxrig_term_text_pointer_target kind;
    /** Owned NUL-terminated UTF-8, or NULL for NONE. */
    char *uri;
    /** URI byte count excluding the terminator. */
    size_t uri_length;
    /** Terminal-lifetime OSC 8 identity, or zero for an inferred URL. */
    uint32_t hyperlink_handle;
    /** Annotation source identity, or zero for OSC 8. */
    uint64_t source_id;
    /** Annotation item identity, or zero for OSC 8. */
    uint64_t item_id;
    /** Revisioned grapheme range for OSC 8, or full inferred URL range. */
    boxrig_text_range range;
} boxrig_term_link_target;

/**
 * Validates bounded HTTP/HTTPS UTF-8 with a nonempty authority.
 * @param policy Policy supplying the existing core UTF-8 validator.
 * @param uri Length-delimited untrusted URI.
 * @param length URI bytes excluding any terminator.
 * @return Whether the URI satisfies the sample's launch contract.
 * @remarks Rejects NUL, whitespace, C0/C1 controls, invalid UTF-8 and other
 * schemes. This function never invokes an external application.
 */
bool boxrig_term_text_policy_validate_uri(
    const boxrig_term_text_policy *policy, const char *uri, size_t length);

/**
 * Resolves and owns one current link without launching it or changing hover.
 * @param policy Policy whose view produced hit.
 * @param hit Exact current pointer hit.
 * @param target_out Receives owned URI and revisioned identity; initialized
 * to NONE on entry. Release a prior value before reusing this output.
 * @return Zero on success (including stale, unsupported or oversized targets),
 * or -1 with errno for invalid arguments or allocation failure.
 * @remarks OSC 8 wins even when its URI is rejected. Inferred URLs use a
 * bounded logical snapshot, independent of wrapping and visual bidi order.
 */
int boxrig_term_text_policy_query_link(
    boxrig_term_text_policy *policy,
    const boxrig_ui_hit_test_result *hit,
    boxrig_term_link_target *target_out);

/**
 * Releases and resets an owned link target.
 * @param target Initialized target to clear, or NULL.
 */
void boxrig_term_link_target_clear(boxrig_term_link_target *target);

/**
 * Refreshes native hand-pointer eligibility at a current content coordinate.
 * @param policy Policy whose view is inspected after model synchronization.
 * @param inside Whether the native pointer is inside the focused content.
 * @param x Current horizontal view coordinate, ignored outside.
 * @param y Current vertical view coordinate, ignored outside.
 * @param hand_out Receives whether a validated launch target exists.
 * @return One when pointer authority changed and one more frame should
 * reflect hover decoration, zero when unchanged, or -1 with errno.
 * @remarks Revalidates text, layout and annotations even without pointer
 * motion. Leaving clears OSC 8 hover. No URI is launched.
 */
int boxrig_term_text_policy_refresh_pointer(
    boxrig_term_text_policy *policy, bool inside,
    boxrig_ui_fixed x, boxrig_ui_fixed y, bool *hand_out);


#endif
