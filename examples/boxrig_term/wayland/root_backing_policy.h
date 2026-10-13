/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WAYLAND_ROOT_BACKING_POLICY_H
#define BOXRIG_TERM_WAYLAND_ROOT_BACKING_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Number of root buffers retained as the current and resize candidate. */
#define BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT ((size_t)2)

/** Default aggregate limit for native Wayland sample root buffers. */
#define BOXRIG_TERM_WAYLAND_ROOT_BYTES_DEFAULT \
    ((size_t)(256u * 1024u * 1024u))

/** Hard aggregate limit for native Wayland sample root buffers. */
#define BOXRIG_TERM_WAYLAND_ROOT_BYTES_MAXIMUM \
    ((size_t)(1024u * 1024u * 1024u))

/** Resource state needed to choose a root resize candidate. */
typedef struct boxrig_term_wayland_root_slot {
    /** Bytes retained by the slot. */
    size_t bytes;
    /** Whether the slot currently owns a native buffer. */
    bool allocated;
    /** Whether the compositor released that native buffer. */
    bool released;
} boxrig_term_wayland_root_slot;

/** Result of a bounded root resize candidate decision. */
typedef enum boxrig_term_wayland_root_candidate_status {
    /** One non-current slot can contain the complete candidate. */
    BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_OK = 0,
    /** Both the current and candidate slots are still in use. */
    BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_BUSY,
    /** The candidate would exceed the aggregate byte limit. */
    BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_LIMIT,
    /** The supplied slot or limit state was invalid. */
    BOXRIG_TERM_WAYLAND_ROOT_CANDIDATE_INVALID
} boxrig_term_wayland_root_candidate_status;

/** Application action for one root-buffer candidate decision. */
typedef enum boxrig_term_wayland_root_update_action {
    /** Populate and commit the selected candidate. */
    BOXRIG_TERM_WAYLAND_ROOT_UPDATE_APPLY = 0,
    /** Retain the update until the compositor releases a buffer. */
    BOXRIG_TERM_WAYLAND_ROOT_UPDATE_RETRY,
    /** Reject this bounded update while retaining the current root. */
    BOXRIG_TERM_WAYLAND_ROOT_UPDATE_REJECT,
    /** Stop because the root candidate state is internally invalid. */
    BOXRIG_TERM_WAYLAND_ROOT_UPDATE_FAIL
} boxrig_term_wayland_root_update_action;

/** A selected non-current slot and its required release action. */
typedef struct boxrig_term_wayland_root_candidate {
    /** Slot index to populate transactionally. */
    size_t index;
    /** Whether the released old buffer must first be destroyed. */
    bool recycle_released;
} boxrig_term_wayland_root_candidate;

/**
 * Chooses a complete root-buffer candidate without mutating current state.
 *
 * @param slots Current bounded slot snapshots.
 * @param slot_count Number of entries in slots; must be exactly two.
 * @param current_index Current slot or slot_count before the first commit.
 * @param maximum_bytes Nonzero aggregate limit up to the hard maximum.
 * @param candidate_bytes Complete candidate allocation size.
 * @param candidate_out Receives the non-current slot on success.
 * @return A bounded candidate decision.
 */
boxrig_term_wayland_root_candidate_status
boxrig_term_wayland_root_choose_candidate(
    const boxrig_term_wayland_root_slot *slots,
    size_t slot_count,
    size_t current_index,
    size_t maximum_bytes,
    size_t candidate_bytes,
    boxrig_term_wayland_root_candidate *candidate_out);

/**
 * Maps a root-buffer candidate decision to application-loop behavior.
 *
 * @param status Candidate decision to classify.
 * @return Whether to apply, retry, reject, or fail the update.
 */
boxrig_term_wayland_root_update_action
boxrig_term_wayland_root_candidate_action(
    boxrig_term_wayland_root_candidate_status status);

/**
 * Returns the background pixel for a native sample root buffer.
 *
 * @param argb Whether the buffer transports alpha.
 * @return Transparent ARGB below the terminal child, otherwise opaque RGB.
 */
uint32_t boxrig_term_wayland_root_clear_pixel(bool argb);

#endif
