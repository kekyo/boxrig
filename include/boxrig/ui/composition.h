/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_COMPOSITION_H
#define BOXRIG_COMPOSITION_H

#include "boxrig/terminal.h"
#include "boxrig/ui/text_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque frontend-owned IME composition and preedit overlay state. */
typedef struct boxrig_composition boxrig_composition;

/** Portable result codes returned by composition operations. */
typedef enum boxrig_composition_status {
    BOXRIG_COMPOSITION_OK = 0,
    BOXRIG_COMPOSITION_INVALID_ARGUMENT,
    BOXRIG_COMPOSITION_OUT_OF_MEMORY,
    BOXRIG_COMPOSITION_LIMIT_EXCEEDED,
    BOXRIG_COMPOSITION_WOULD_BLOCK
} boxrig_composition_status;

/** Destination that atomically accepts one committed UTF-8 string. */
typedef boxrig_status (*boxrig_composition_commit_sink)(
    const char *text,
    size_t text_length,
    void *context);

/** Distinct state transitions produced by a platform input method. */
typedef enum boxrig_composition_event_type {
    BOXRIG_COMPOSITION_PREEDIT_UPDATE = 0,
    BOXRIG_COMPOSITION_PREEDIT_COMPLETE,
    BOXRIG_COMPOSITION_COMMIT,
    BOXRIG_COMPOSITION_CANCEL
} boxrig_composition_event_type;

/** Portable preedit feedback flags retained on conversion segments. */
typedef enum boxrig_composition_feedback {
    BOXRIG_COMPOSITION_FEEDBACK_NONE = 0,
    BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE = 1u << 0,
    BOXRIG_COMPOSITION_FEEDBACK_REVERSE = 1u << 1,
    BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT = 1u << 2,
    BOXRIG_COMPOSITION_FEEDBACK_PRIMARY = 1u << 3,
    BOXRIG_COMPOSITION_FEEDBACK_SECONDARY = 1u << 4,
    BOXRIG_COMPOSITION_FEEDBACK_TERTIARY = 1u << 5,
    BOXRIG_COMPOSITION_FEEDBACK_CONVERTED = 1u << 6,
    BOXRIG_COMPOSITION_FEEDBACK_TARGET = 1u << 7,
    BOXRIG_COMPOSITION_FEEDBACK_ERROR = 1u << 8
} boxrig_composition_feedback;

/** One conversion or visual-feedback segment in UTF-8 byte coordinates. */
typedef struct boxrig_composition_segment {
    /** First byte in the event text. */
    size_t text_offset;
    /** Number of bytes in the segment. */
    size_t text_length;
    /** Bitwise combination of boxrig_composition_feedback values. */
    uint32_t feedback;
} boxrig_composition_segment;

/** One platform-independent composition transition. */
typedef struct boxrig_composition_event {
    /** Transition kind. */
    boxrig_composition_event_type type;
    /** Length-delimited UTF-8 preedit or committed text. */
    const char *text;
    /** Number of bytes in text. */
    size_t text_length;
    /** Ordered non-overlapping preedit segments. */
    const boxrig_composition_segment *segments;
    /** Number of elements in segments. */
    size_t segment_count;
    /** First selected UTF-8 byte in preedit text. */
    size_t selection_offset;
    /** Number of selected UTF-8 bytes. */
    size_t selection_length;
    /** UTF-8 byte position before which the preedit caret is drawn. */
    size_t caret_offset;
    /** Whether the preedit caret is visible. */
    bool caret_visible;
} boxrig_composition_event;

/** Bounded storage retained by one composition. */
typedef struct boxrig_composition_limits {
    /** Maximum UTF-8 bytes in one preedit or commit transition. */
    size_t maximum_text_bytes;
    /** Maximum conversion segments in one preedit update. */
    size_t maximum_segments;
} boxrig_composition_limits;

/** One placed extended grapheme cluster in the transient overlay. */
typedef struct boxrig_composition_cluster_view {
    /** Borrowed UTF-8 cluster bytes owned by the composition. */
    const char *text;
    /** Number of bytes in text. */
    size_t text_length;
    /** Offset of text in the complete preedit string. */
    size_t text_offset;
    /** Number of terminal columns occupied by the cluster. */
    size_t column_count;
    /** Viewport row where the cluster is placed. */
    size_t row;
    /** Visual viewport column where the cluster is placed. */
    size_t column;
    /** Feedback inherited from the containing conversion segment. */
    uint32_t feedback;
    /** Whether any part of this cluster intersects the selection. */
    bool selected;
    /** Whether the placed cluster is inside the visible grid. */
    bool visible;
} boxrig_composition_cluster_view;

/** Candidate-window anchor derived from the preedit caret. */
typedef struct boxrig_composition_candidate_view {
    /** Whether the candidate anchor is currently visible. */
    bool visible;
    /** Viewport row containing the caret. */
    size_t row;
    /** Visual viewport column containing the caret. */
    size_t column;
    /** Caret insertion x coordinate in output pixels. */
    size_t spot_x;
    /** Text baseline y coordinate in output pixels. */
    size_t spot_y;
    /** Left edge of the cell excluded from candidate overlap. */
    size_t exclude_x;
    /** Top edge of the cell excluded from candidate overlap. */
    size_t exclude_y;
    /** Width of the excluded caret cell in pixels. */
    unsigned int exclude_width;
    /** Height of the excluded caret cell in pixels. */
    unsigned int exclude_height;
} boxrig_composition_candidate_view;

/** Borrowed snapshot of the current composition state. */
typedef struct boxrig_composition_view {
    /** Last accepted platform transition. */
    boxrig_composition_event_type last_event;
    /** Whether preedit text is currently being edited. */
    bool active;
    /** Whether the overlay is visible in the active viewport. */
    bool visible;
    /** Whether one atomic commit is waiting for outbound capacity. */
    bool commit_pending;
    /** Whether the frontend still owns keyboard focus. */
    bool focused;
    /** Whether terminal shutdown ended this lifecycle. */
    bool terminated;
    /** Borrowed complete UTF-8 preedit text. */
    const char *text;
    /** Number of bytes in text. */
    size_t text_length;
    /** Borrowed conversion segments. */
    const boxrig_composition_segment *segments;
    /** Number of elements in segments. */
    size_t segment_count;
    /** First selected byte in text. */
    size_t selection_offset;
    /** Number of selected bytes. */
    size_t selection_length;
    /** UTF-8 byte caret position. */
    size_t caret_offset;
    /** Whether the preedit caret is visible. */
    bool caret_visible;
    /** Terminal row used as the preedit insertion anchor. */
    size_t anchor_row;
    /** Logical terminal column used as the insertion anchor. */
    size_t anchor_logical_column;
    /** Visual terminal column resolved through the row layout. */
    size_t anchor_visual_column;
    /** Direction of the shaping run containing the terminal cursor. */
    boxrig_text_direction direction;
    /** Number of placed grapheme clusters. */
    size_t cluster_count;
    /** Candidate-window anchor at the current caret. */
    boxrig_composition_candidate_view candidate;
    /** Nonzero revision of text, lifecycle, or placement state. */
    uint64_t revision;
} boxrig_composition_view;

/**
 * Returns conservative limits for frontend preedit storage.
 *
 * @return Limits of 4096 UTF-8 bytes and 256 conversion segments.
 */
boxrig_composition_limits boxrig_composition_limits_default(void);

/**
 * Creates frontend-owned transient composition state.
 *
 * @param terminal Terminal receiving committed UTF-8 text only.
 * @param limits Bounded text and segment storage.
 * @param composition_out Receives the allocated composition.
 * @return A portable composition status.
 */
boxrig_composition_status boxrig_composition_create(
    boxrig_terminal *terminal,
    boxrig_composition_limits limits,
    boxrig_composition **composition_out);

/**
 * Replaces the destination for subsequent committed text.
 *
 * @param composition Composition to configure.
 * @param sink Atomic destination callback.
 * @param context Opaque context passed to sink.
 * @return BOXRIG_COMPOSITION_WOULD_BLOCK while an accepted commit is pending,
 * or another composition status.
 *
 * @remarks The sink must either accept the complete string or return
 * BOXRIG_STATUS_WOULD_BLOCK without accepting any bytes. A blocked string is
 * owned by the composition and retried through the same sink and context.
 */
boxrig_composition_status boxrig_composition_set_commit_sink(
    boxrig_composition *composition,
    boxrig_composition_commit_sink sink,
    void *context);

/**
 * Releases transient preedit and pending-commit storage.
 *
 * @param composition Composition to release, or NULL.
 */
void boxrig_composition_destroy(boxrig_composition *composition);

/**
 * Applies one platform-independent preedit, commit, or cancel transition.
 *
 * @param composition Composition to mutate.
 * @param event Length-delimited transition data.
 * @return BOXRIG_COMPOSITION_WOULD_BLOCK when an accepted atomic commit is
 * waiting for terminal outbound capacity, or another composition status.
 *
 * @remarks Preedit updates, completion, and cancellation never mutate terminal
 * cells, scrollback, or outbound bytes. Only a commit sends literal UTF-8 text.
 */
boxrig_composition_status boxrig_composition_apply_event(
    boxrig_composition *composition,
    const boxrig_composition_event *event);

/**
 * Retries an accepted commit without duplicating its UTF-8 bytes.
 *
 * @param composition Composition with an optional pending commit.
 * @return BOXRIG_COMPOSITION_WOULD_BLOCK while capacity is unavailable, or a
 * portable composition status.
 */
boxrig_composition_status boxrig_composition_retry_commit(
    boxrig_composition *composition);

/**
 * Discards one complete backpressured commit without invoking its sink.
 *
 * @param composition Composition whose pending atomic commit is cancelled.
 * @return A portable composition status.
 *
 * @remarks This operation is for native transaction cancellation. It never
 * forwards the discarded text to another sink and leaves the composition
 * reusable.
 */
boxrig_composition_status boxrig_composition_cancel_pending_commit(
    boxrig_composition *composition);

/**
 * Updates frontend focus and cancels preedit when focus is lost.
 *
 * @param composition Composition to mutate.
 * @param focused Whether the terminal view owns keyboard focus.
 * @return A portable composition status.
 */
boxrig_composition_status boxrig_composition_focus_changed(
    boxrig_composition *composition,
    bool focused);

/**
 * Repositions preedit from the current cursor and row layout.
 *
 * @param composition Composition to synchronize.
 * @param cursor_row_layout Current immutable layout for the cursor row. This
 * may be NULL only when no preedit is active.
 * @param cell_width Positive output cell width in pixels.
 * @param cell_height Positive output cell height in pixels.
 * @param baseline Positive text baseline offset no greater than cell_height.
 * @return A portable composition status.
 *
 * @remarks Resize, cursor movement, scrolling, and viewport movement recompute
 * placement. A primary/alternate screen switch cancels the composition.
 */
boxrig_composition_status boxrig_composition_synchronize(
    boxrig_composition *composition,
    const boxrig_text_layout *cursor_row_layout,
    unsigned int cell_width,
    unsigned int cell_height,
    unsigned int baseline);

/**
 * Ends composition permanently for terminal/frontend shutdown.
 *
 * @param composition Composition to terminate.
 * @return A portable composition status.
 */
boxrig_composition_status boxrig_composition_terminate(
    boxrig_composition *composition);

/**
 * Reads a borrowed snapshot valid until the next composition mutation.
 *
 * @param composition Composition to inspect.
 * @param view_out Receives the snapshot.
 * @return A portable composition status.
 */
boxrig_composition_status boxrig_composition_get_view(
    const boxrig_composition *composition,
    boxrig_composition_view *view_out);

/**
 * Reads one placed grapheme cluster.
 *
 * @param composition Composition to inspect.
 * @param cluster_index Zero-based cluster index in logical preedit order.
 * @param cluster_out Receives the borrowed cluster view.
 * @return A portable composition status.
 */
boxrig_composition_status boxrig_composition_get_cluster(
    const boxrig_composition *composition,
    size_t cluster_index,
    boxrig_composition_cluster_view *cluster_out);

#ifdef __cplusplus
}
#endif

#endif
