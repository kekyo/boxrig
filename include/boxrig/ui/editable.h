/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_EDITABLE_H
#define BOXRIG_UI_EDITABLE_H

#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default maximum UTF-8 bytes copied into one editable context. */
#define BOXRIG_UI_EDITABLE_CONTEXT_CAPACITY_DEFAULT ((size_t)(64u * 1024u))

/** Hard maximum UTF-8 bytes copied into one editable context. */
#define BOXRIG_UI_EDITABLE_CONTEXT_CAPACITY_MAX ((size_t)(1024u * 1024u))

/** Default maximum UTF-8 bytes in one selection or replacement. */
#define BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_DEFAULT ((size_t)(4u * 1024u))

/** Hard maximum UTF-8 bytes in one selection or replacement. */
#define BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_MAX ((size_t)(64u * 1024u))

/** Default maximum uncompleted replacement requests. */
#define BOXRIG_UI_EDITABLE_PENDING_REPLACEMENTS_DEFAULT ((size_t)8u)

/** Hard maximum uncompleted replacement requests. */
#define BOXRIG_UI_EDITABLE_PENDING_REPLACEMENTS_MAX ((size_t)256u)

/** Opaque application-document cache and replacement endpoint owned by a view. */
typedef struct boxrig_ui_editable_document boxrig_ui_editable_document;

/** Absolute UTF-8 byte range in an application-owned editable document. */
typedef struct boxrig_ui_editable_range {
    /** Absolute byte offset from the start of the application document. */
    uint64_t offset;
    /** Number of UTF-8 bytes in the half-open range. */
    size_t length;
} boxrig_ui_editable_range;

/** Bounded storage limits for one editable-document endpoint. */
typedef struct boxrig_ui_editable_limits {
    /** Maximum UTF-8 bytes copied from one published context. */
    size_t maximum_context_bytes;
    /** Maximum UTF-8 bytes in a selection or replacement. */
    size_t maximum_selection_bytes;
    /** Maximum queued replacement requests; zero applies backpressure. */
    size_t maximum_pending_replacements;
} boxrig_ui_editable_limits;

/** Application input for one immutable editable-document snapshot. */
typedef struct boxrig_ui_editable_document_snapshot_input {
    /** Nonzero application-defined document identity. */
    uint64_t document_identity;
    /** Nonzero application-defined content revision. */
    uint64_t document_revision;
    /** UTF-8 context containing the selection; NULL is allowed only when empty. */
    const char *context;
    /** Number of bytes in context. */
    size_t context_length;
    /** Absolute document byte offset represented by context[0]. */
    uint64_t context_offset;
    /** Absolute selected range; zero length represents an insertion position. */
    boxrig_ui_editable_range selection;
} boxrig_ui_editable_document_snapshot_input;

/** Application-owned surrounding context and directed selection. */
typedef struct boxrig_ui_editable_context_input {
    /** Nonzero application-defined document identity. */
    uint64_t document_identity;
    /** Nonzero application-defined content revision, independent of selection. */
    uint64_t document_revision;
    /** Committed UTF-8 without preedit; NULL is allowed only when empty. */
    const char *context;
    /** Number of bytes in context. */
    size_t context_length;
    /** Absolute document byte offset represented by context[0]. */
    uint64_t context_offset;
    /** Absolute UTF-8 insertion boundary within or at either end of context. */
    uint64_t cursor;
    /** Absolute UTF-8 selection anchor; equals cursor for an empty selection. */
    uint64_t anchor;
} boxrig_ui_editable_context_input;

/** Borrowed immutable view of the cached editable-document snapshot. */
typedef struct boxrig_ui_editable_document_snapshot {
    /** Application-defined document identity. */
    uint64_t document_identity;
    /** Application-defined content revision. */
    uint64_t document_revision;
    /** Borrowed length-delimited UTF-8 context owned by the view. */
    const char *context;
    /** Number of bytes in context. */
    size_t context_length;
    /** Absolute document byte offset represented by context[0]. */
    uint64_t context_offset;
    /** Absolute range between cursor and anchor; may be empty. */
    boxrig_ui_editable_range selection;
    /** Absolute UTF-8 insertion boundary. */
    uint64_t cursor;
    /** Absolute UTF-8 selection anchor, preserving the selection direction. */
    uint64_t anchor;
    /** Nonzero endpoint-assigned generation including context and selection. */
    uint64_t input_generation;
} boxrig_ui_editable_document_snapshot;

/** One application-visible atomic document replacement request. */
typedef struct boxrig_ui_editable_replacement_request {
    /** Nonzero view-assigned request identity. */
    uint64_t request_id;
    /** Fixed application document identity. */
    uint64_t document_identity;
    /** Fixed document revision on which the replacement is based. */
    uint64_t base_document_revision;
    /** Fixed input generation on which the replacement is based. */
    uint64_t base_input_generation;
    /** Fixed absolute UTF-8 byte range to replace. */
    boxrig_ui_editable_range range;
    /** Borrowed length-delimited UTF-8 replacement owned by the view. */
    const char *replacement;
    /** Number of bytes in replacement; zero represents deletion. */
    size_t replacement_length;
} boxrig_ui_editable_replacement_request;

/** Application disposition of one pulled replacement request. */
typedef enum boxrig_ui_editable_replacement_outcome {
    /** The fixed range was replaced exactly once. */
    BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED = 0,
    /** The fixed identity or base revision was no longer current. */
    BOXRIG_UI_EDITABLE_REPLACEMENT_STALE,
    /** Application policy rejected the otherwise current replacement. */
    BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED
} boxrig_ui_editable_replacement_outcome;

/** Synchronous edit expressed in absolute UTF-8 byte positions. */
typedef struct boxrig_ui_editable_edit {
    /** Generation captured with the context used by the input method. */
    uint64_t input_generation;
    /** Scalar-aligned range to replace; an empty range inserts text. */
    boxrig_ui_editable_range range;
    /** Length-delimited UTF-8 replacement; NULL is allowed when empty. */
    const char *replacement;
    /** Number of UTF-8 bytes to insert. */
    size_t replacement_length;
    /** Absolute cursor in the resulting document. */
    uint64_t cursor_after;
    /** Absolute anchor in the resulting document. */
    uint64_t anchor_after;
} boxrig_ui_editable_edit;

/** Document capabilities before intersecting them with native IME support. */
typedef struct boxrig_ui_editable_capabilities {
    /** A finite, validated application document context is available. */
    bool context_available;
    /** A synchronous owner is registered for the available document. */
    bool synchronous_edit_available;
    /** A nonempty grapheme selection can start asynchronous reconversion. */
    bool reconversion_available;
    /** Another edit or reconversion currently owns the endpoint. */
    bool busy;
} boxrig_ui_editable_capabilities;

/**
 * Applies an already prepared replacement to the application document.
 * @param request Fixed identity, revision, generation and grapheme-aligned edit.
 * @param proposed Fully prepared resulting context, including the next revision.
 * @param context Borrowed owner state registered with the endpoint.
 * @return OK only after applying the replacement exactly once and adopting
 * proposed->document_revision; STALE for a different document or revision,
 * UNAVAILABLE for rejection, BUSY for temporary backpressure, or another UI
 * failure. Every failure must leave the application document unchanged.
 * @remarks Pointers are borrowed only during the call. The owner must not
 * wait for I/O, run a nested event loop, mutate this endpoint, or destroy its
 * view. Read-only endpoint inspection remains available. The proposed body
 * revision is base_document_revision + 1; its generation is also advanced.
 * All endpoint storage is prepared before this callback, so acceptance cannot
 * subsequently fail because of an allocation or limit check.
 */
typedef boxrig_ui_status (*boxrig_ui_editable_apply_handler)(
    const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed,
    void *context);

/** Explicit application completion for one pulled replacement request. */
typedef struct boxrig_ui_editable_replacement_completion {
    /** Request identity previously returned by the pull operation. */
    uint64_t request_id;
    /** Accepted, stale, or rejected disposition. */
    boxrig_ui_editable_replacement_outcome outcome;
    /** Nonzero resulting revision for accepted, otherwise zero. */
    uint64_t new_document_revision;
} boxrig_ui_editable_replacement_completion;

/** Current state of the single replacement transaction owned by a view. */
typedef enum boxrig_ui_editable_reconversion_state {
    /** No reconversion transaction is active. */
    BOXRIG_UI_EDITABLE_RECONVERSION_NONE = 0,
    /** Identity, revision, and replacement range have been fixed. */
    BOXRIG_UI_EDITABLE_RECONVERSION_ACTIVE,
    /** A newer snapshot invalidated the fixed transaction. */
    BOXRIG_UI_EDITABLE_RECONVERSION_STALE,
    /** One replacement is waiting for application completion. */
    BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION
} boxrig_ui_editable_reconversion_state;

/**
 * Returns deterministic default editable-document limits.
 *
 * @return Public default context, selection, and pending-request limits.
 */
boxrig_ui_editable_limits boxrig_ui_editable_limits_default(void);

/**
 * Returns the editable-document endpoint owned by one view.
 *
 * @param view View to inspect.
 * @return Borrowed endpoint with the same lifetime as view, or NULL.
 */
boxrig_ui_editable_document *boxrig_ui_view_editable_document(
    boxrig_ui_view *view);

/**
 * Reconfigures bounded editable-document storage transactionally.
 *
 * @param document Endpoint to configure.
 * @param limits Limits no greater than the published hard maxima.
 * @return A portable UI status. Existing data is preserved on failure.
 */
boxrig_ui_status boxrig_ui_editable_document_set_limits(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_limits limits);

/**
 * Copies one validated application-owned document snapshot into the view.
 *
 * @param document Endpoint receiving the snapshot.
 * @param input Identity, revision, context, and selection or insertion position.
 * @return A portable UI status. The previous snapshot is preserved on failure.
 *
 * @remarks Context and selection are never truncated. Both selection ends
 * must be UTF-8 extended-grapheme-cluster boundaries. Publishing a different
 * input state while reconversion is active makes that transaction stale
 * without invoking an application callback. Cursor is the selection end;
 * anchor is the selection start.
 */
boxrig_ui_status boxrig_ui_editable_document_publish(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_document_snapshot_input *input);

/**
 * Publishes bounded context with an insertion position and directed selection.
 *
 * @param document Endpoint receiving an owned copy of the context.
 * @param input Identity, body revision, UTF-8 window and absolute positions.
 * @return A portable UI status, preserving the old state on failure.
 * @remarks Cursor and anchor must be Unicode scalar boundaries. Native IMEs
 * may address part of a grapheme; reconversion still requires a nonempty
 * grapheme-aligned selection. Selection and window changes may use the same
 * body revision, but overlapping bytes of that revision must agree. Any
 * changed input state advances the generation and invalidates active
 * reconversion. Repeating an identical publication preserves the generation.
 */
boxrig_ui_status boxrig_ui_editable_document_publish_context(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_context_input *input);

/**
 * Reads a bounded context containing both cursor and anchor.
 *
 * @param document Endpoint to inspect.
 * @param maximum_bytes Maximum bytes in the returned context window.
 * @param snapshot_out Receives a borrowed snapshot with absolute positions.
 * @return OK, UNAVAILABLE without a document, LIMIT_EXCEEDED if the complete
 * selection and its containing graphemes cannot fit, or INVALID_ARGUMENT.
 * @remarks The window is cropped only at grapheme boundaries. Its byte count
 * may be less than maximum_bytes; it is not necessarily NUL terminated at
 * that count. The borrowed memory expires on the next successful mutation.
 * Outputs are unchanged on failure. Zero capacity can represent an empty
 * document or an insertion position at a grapheme boundary.
 */
boxrig_ui_status boxrig_ui_editable_document_get_context(
    const boxrig_ui_editable_document *document,
    size_t maximum_bytes,
    boxrig_ui_editable_document_snapshot *snapshot_out);

/**
 * Invalidates native input captured before a focus change or cancellation.
 * @param document Endpoint whose input generation is advanced.
 * @return A portable UI status; cached text and application requests remain.
 */
boxrig_ui_status boxrig_ui_editable_document_invalidate_input(
    boxrig_ui_editable_document *document);

/**
 * Reads the generation, including when no document is currently published.
 * @param document Endpoint to inspect.
 * @return A nonzero generation, or zero for NULL.
 */
uint64_t boxrig_ui_editable_document_input_generation(
    const boxrig_ui_editable_document *document);

/**
 * Registers the owner of synchronous IME edits and invalidates captured input.
 * @param document Endpoint to configure.
 * @param handler Owner callback, or NULL to remove synchronous editing.
 * @param context Borrowed owner state; must be NULL when handler is NULL.
 * @return A portable UI status. Identical registration is a no-op.
 */
boxrig_ui_status boxrig_ui_editable_document_set_apply_handler(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_apply_handler handler,
    void *context);

/**
 * Reads context, synchronous editing and reconversion availability separately.
 * @param document Endpoint to inspect, or NULL for no capabilities.
 * @return Document-side capabilities; native protocol support is independent.
 */
boxrig_ui_editable_capabilities boxrig_ui_editable_document_capabilities(
    const boxrig_ui_editable_document *document);

/**
 * Validates, prepares and synchronously applies one IME edit.
 * @param document Endpoint with a registered synchronous owner.
 * @param edit Captured generation, scalar range, replacement and new positions.
 * @return OK after owner acceptance, STALE for an expired generation, BUSY for
 * another transaction, UNAVAILABLE without an owner, or a validation/owner
 * failure. Failures preserve the snapshot and never emit terminal bytes.
 * @remarks Partial-grapheme edits become replacements of the enclosing whole
 * graphemes, retaining untouched prefix/suffix scalars. Both the normalized
 * replacement and resulting context must fit their configured limits. An
 * accepted edit advances the generation, so replaying it returns STALE.
 */
boxrig_ui_status boxrig_ui_editable_document_apply_edit(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_edit *edit);

/**
 * Removes the cached document snapshot without touching a queued request.
 *
 * @param document Endpoint to clear.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_editable_document_clear(
    boxrig_ui_editable_document *document);

/**
 * Reads the current callback-free cached snapshot.
 *
 * @param document Endpoint to inspect.
 * @param snapshot_out Receives a borrowed snapshot.
 * @return True when a snapshot is available.
 */
bool boxrig_ui_editable_document_get_snapshot(
    const boxrig_ui_editable_document *document,
    boxrig_ui_editable_document_snapshot *snapshot_out);

/**
 * Fixes the current snapshot selection as one reconversion transaction.
 *
 * @param document Endpoint owning the transaction.
 * @return UNAVAILABLE without a nonempty grapheme-aligned selection, or a
 * portable UI status.
 */
boxrig_ui_status boxrig_ui_editable_document_begin_reconversion(
    boxrig_ui_editable_document *document);

/**
 * Replaces the fixed range with a validated range in the same context.
 *
 * @param document Endpoint owning the active transaction.
 * @param range Absolute UTF-8 byte range confirmed by a native input method.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_editable_document_confirm_reconversion_range(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_range range);

/**
 * Cancels an active or stale transaction before a request was accepted.
 *
 * @param document Endpoint owning the transaction.
 * @return BOXRIG_UI_BUSY when a request already awaits completion, otherwise
 * a portable UI status.
 */
boxrig_ui_status boxrig_ui_editable_document_cancel_reconversion(
    boxrig_ui_editable_document *document);

/**
 * Atomically queues one committed replacement for the fixed range.
 *
 * @param text Length-delimited UTF-8 replacement, or NULL only when empty.
 * @param text_length Number of bytes in text.
 * @param context A boxrig_ui_editable_document pointer.
 * @return BOXRIG_STATUS_WOULD_BLOCK without accepting bytes when the queue
 * limit is zero, BOXRIG_STATUS_STALE for an invalidated transaction, or
 * another boxrig status.
 *
 * @remarks This function has the boxrig_composition_commit_sink signature.
 * A blocked composition may retry the exact complete string through the same
 * endpoint. An accepted request is never forwarded to terminal output.
 */
boxrig_status boxrig_ui_editable_document_commit_reconversion(
    const char *text,
    size_t text_length,
    void *context);

/**
 * Pulls the oldest unobserved replacement request exactly once.
 *
 * @param document Endpoint to inspect.
 * @param request_out Receives a borrowed request valid until completion.
 * @return True when one request was returned.
 */
bool boxrig_ui_editable_document_take_replacement(
    boxrig_ui_editable_document *document,
    boxrig_ui_editable_replacement_request *request_out);

/**
 * Completes one previously pulled replacement request exactly once.
 *
 * @param document Endpoint owning the request.
 * @param completion Application disposition and optional resulting revision.
 * @return BOXRIG_UI_STALE for an unknown or duplicate request, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_ui_editable_document_complete_replacement(
    boxrig_ui_editable_document *document,
    const boxrig_ui_editable_replacement_completion *completion);

/**
 * Reads the current replacement transaction state.
 *
 * @param document Endpoint to inspect.
 * @return Current state, or BOXRIG_UI_EDITABLE_RECONVERSION_NONE for NULL.
 */
boxrig_ui_editable_reconversion_state
boxrig_ui_editable_document_reconversion_state(
    const boxrig_ui_editable_document *document);

#ifdef __cplusplus
}
#endif

#endif
