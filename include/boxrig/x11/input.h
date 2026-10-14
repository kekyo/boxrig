/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_INPUT_H
#define BOXRIG_X11_INPUT_H

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/editable.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/view.h"

#include <X11/Xlib.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque X11-to-logical-input adapter used by the X11 sample. */
typedef struct boxrig_x11_input_adapter boxrig_x11_input_adapter;

/**
 * Creates an X11 input adapter and its UTF-8 input context.
 *
 * @param display X11 connection used by both windows.
 * @param client_window Caller-owned window in which XIM may display data or
 * create subwindows.
 * @param focus_window Terminal view window that receives composed key input.
 * @param terminal Terminal that receives logical input events.
 * @param cell_width Positive terminal cell width in pixels.
 * @param cell_height Positive terminal cell height in pixels.
 * @param paste_capacity Maximum accepted selection payload in bytes.
 * @param paste_chunk_size Preferred bytes per complete UTF-8 paste chunk (at least one scalar).
 * @param adapter_out Receives the new adapter.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_input_adapter_create(
    Display *display,
    Window client_window,
    Window focus_window,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t paste_capacity,
    size_t paste_chunk_size,
    boxrig_x11_input_adapter **adapter_out);

/**
 * Attaches application-owned surrounding text and synchronous IME edits.
 * @param adapter Adapter to configure.
 * @param document Borrowed endpoint, or NULL to detach it. The endpoint must
 * outlive the attachment; native components attach their view's endpoint.
 * @return Zero on success, or -1 for invalid input or an active document owner.
 * @remarks IME commits use a published document even when the XIM server
 * cannot retrieve surrounding text. An unpublished endpoint keeps ordinary
 * terminal input. Refused document edits never fall through to terminal input.
 */
int boxrig_x11_input_adapter_set_editable_document(
    boxrig_x11_input_adapter *adapter,
    boxrig_ui_editable_document *document);

/**
 * Reports whether the current XIM accepts string conversion callbacks.
 * @param adapter Adapter to inspect, or NULL.
 * @return True after the XIM advertises and accepts the optional callback.
 * @remarks This is independent of document-side capabilities. Requests use
 * character-relative positions in a bounded 4000-byte UTF-8 context window;
 * only XIMForwardChar and XIMBackwardChar are supported. The XIM implementation
 * must honor Xlib's callback result and buffer-ownership contract.
 */
bool boxrig_x11_input_adapter_string_conversion_supported(
    const boxrig_x11_input_adapter *adapter);

/**
 * Installs an application key router used before terminal input encoding.
 *
 * @param adapter Adapter to configure.
 * @param router Router callback, or NULL to disable application routing.
 * @param context Opaque callback context, which must be NULL without a router.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_x11_input_adapter_set_key_router(
    boxrig_x11_input_adapter *adapter,
    boxrig_input_key_router router,
    void *context);

/**
 * Installs an application pointer router used before terminal mouse encoding.
 *
 * @param adapter Adapter to configure.
 * @param view Borrowed view used to resolve the native event coordinate, or
 * NULL when disabling routing. It must belong to adapter's terminal.
 * @param router Router callback, or NULL to disable application routing.
 * @param context Opaque callback context, which must be NULL without a router.
 * @return Zero on success, or -1 for invalid input or an active capture.
 */
int boxrig_x11_input_adapter_set_mouse_router(
    boxrig_x11_input_adapter *adapter,
    boxrig_ui_view *view,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Reports copy and paste targets implemented by this adapter.
 *
 * @param adapter Adapter to inspect.
 * @return A complete portable capability snapshot, or no capabilities for
 * NULL.
 */
boxrig_ui_view_action_capabilities
boxrig_x11_input_adapter_view_action_capabilities(
    const boxrig_x11_input_adapter *adapter);

/**
 * Starts one shortcut-independent portable clipboard action.
 *
 * @param adapter Adapter that owns the native action transaction.
 * @param view View providing an immutable copy snapshot. This may be NULL for
 * paste but is required for copy.
 * @param action Copy or paste action with exactly one target.
 * @return A portable UI status. A second pending paste returns BOXRIG_UI_BUSY.
 *
 * @remarks X11 ownership or conversion begins after the server-timestamp
 * PropertyNotify event is forwarded to the adapter. A successful copy offer
 * remains immutable until SelectionClear, replacement for the same target,
 * quiesce, or destruction. X11 stores the owner identity rather than the
 * payload, so the text is not guaranteed to survive adapter or process
 * destruction without an external clipboard manager.
 */
boxrig_ui_status boxrig_x11_input_adapter_apply_view_action(
    boxrig_x11_input_adapter *adapter,
    boxrig_ui_view *view,
    const boxrig_ui_view_action *action);

/**
 * Replaces the bounded X11 selection-transfer resources.
 *
 * @param adapter Adapter to configure.
 * @param chunk_size Requested maximum bytes placed in one direct property or
 * INCR chunk. The adapter clamps this value to the X server request limit.
 * @param maximum_active_transfers Maximum simultaneous outgoing INCR
 * transfers.
 * @param maximum_aggregate_bytes Maximum sum of payload bytes retained by
 * simultaneous outgoing INCR transfers. One transfer is bounded by the same
 * value.
 * @return Zero on success, or -1 for invalid input, allocation failure, or an
 * active native selection transaction.
 */
int boxrig_x11_input_adapter_set_clipboard_transfer_limits(
    boxrig_x11_input_adapter *adapter,
    size_t chunk_size,
    size_t maximum_active_transfers,
    size_t maximum_aggregate_bytes);

/**
 * Relinquishes owned selections and cancels native selection transactions.
 *
 * @param adapter Adapter to stop, or NULL.
 * @return Zero on success. NULL is accepted.
 */
int boxrig_x11_input_adapter_quiesce(boxrig_x11_input_adapter *adapter);

/**
 * Updates the cell dimensions used for pointer hit testing.
 *
 * @param adapter Adapter to configure.
 * @param cell_width Positive terminal cell width in pixels.
 * @param cell_height Positive terminal cell height in pixels.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_x11_input_adapter_set_cell_size(
    boxrig_x11_input_adapter *adapter,
    unsigned int cell_width,
    unsigned int cell_height);

/**
 * Sets the terminal viewport origin in input-window coordinates.
 *
 * @param adapter Adapter to configure.
 * @param x Horizontal input-window origin.
 * @param y Vertical input-window origin.
 * @return Zero on success, or -1 for an invalid adapter.
 *
 * @remarks Pointer coordinates subtract this origin. XIM candidate placement
 * adds it to the component-local composition spot.
 */
int boxrig_x11_input_adapter_set_viewport_origin(
    boxrig_x11_input_adapter *adapter,
    int x,
    int y);

/**
 * Returns the frontend-owned composition associated with this adapter.
 *
 * @param adapter Adapter to inspect.
 * @return Borrowed composition, or NULL for invalid input.
 */
boxrig_composition *boxrig_x11_input_adapter_composition(
    boxrig_x11_input_adapter *adapter);

/**
 * Starts one callback-style XIM preedit transaction.
 *
 * @param adapter Adapter receiving the callback.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_input_adapter_preedit_start(
    boxrig_x11_input_adapter *adapter);

/**
 * Applies an XIM character-indexed preedit replacement.
 *
 * @param adapter Adapter receiving the callback.
 * @param draw XIM replacement, feedback, and caret payload.
 * @return Zero on success, or -1 on invalid or rejected input.
 */
int boxrig_x11_input_adapter_preedit_draw(
    boxrig_x11_input_adapter *adapter,
    const XIMPreeditDrawCallbackStruct *draw);

/**
 * Applies a callback-style XIM caret transition.
 *
 * @param adapter Adapter receiving the callback.
 * @param caret XIM caret payload.
 * @return Zero on success, or -1 on invalid or rejected input.
 */
int boxrig_x11_input_adapter_preedit_caret(
    boxrig_x11_input_adapter *adapter,
    XIMPreeditCaretCallbackStruct *caret);

/**
 * Completes and removes the current transient XIM preedit.
 *
 * @param adapter Adapter receiving the callback.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_input_adapter_preedit_done(
    boxrig_x11_input_adapter *adapter);

/**
 * Cancels and removes the current transient XIM preedit.
 *
 * @param adapter Adapter receiving the cancellation.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_input_adapter_preedit_cancel(
    boxrig_x11_input_adapter *adapter);

/**
 * Commits one length-delimited XIM UTF-8 result atomically.
 *
 * @param adapter Adapter receiving committed text.
 * @param text UTF-8 result bytes.
 * @param text_length Number of bytes in text.
 * @return Zero when accepted, including while awaiting output capacity, or
 * -1 on invalid input or failure.
 */
int boxrig_x11_input_adapter_commit_utf8(
    boxrig_x11_input_adapter *adapter,
    const char *text,
    size_t text_length);

/**
 * Updates the native candidate anchor from the placed composition caret.
 *
 * @param adapter Adapter to synchronize after composition rendering.
 * @return Zero on success or when no candidate is visible, or -1 on failure.
 */
int boxrig_x11_input_adapter_synchronize_candidate(
    boxrig_x11_input_adapter *adapter);

/**
 * Converts one X11 event to a logical terminal input event.
 *
 * @param adapter Adapter to run.
 * @param event Event obtained from XNextEvent().
 * @return One when handled, zero when unrelated or input is blocked, or -1 on
 * failure.
 *
 * @remarks Offer every event removed by the caller-owned loop so Xlib can
 * filter XIM protocol events whose native target differs from focus_window.
 * Ordinary parent and sibling events remain unrelated.
 */
int boxrig_x11_input_adapter_process_event(
    boxrig_x11_input_adapter *adapter,
    const XEvent *event);

/**
 * Queues normalized UTF-8 as one streamed paste transaction.
 *
 * @param adapter Adapter to run.
 * @param bytes Untrusted length-delimited clipboard bytes.
 * @param length Number of bytes in bytes.
 * @return Zero when accepted, or -1 for invalid input or failure.
 */
int boxrig_x11_input_adapter_queue_paste(
    boxrig_x11_input_adapter *adapter,
    const uint8_t *bytes,
    size_t length);

/**
 * Retries the currently blocked logical event or paste phase.
 *
 * @param adapter Adapter to run.
 * @return Zero while pending or after completion, or -1 on failure.
 */
int boxrig_x11_input_adapter_retry(boxrig_x11_input_adapter *adapter);

/**
 * Reports whether a logical event is waiting for terminal output capacity.
 *
 * @param adapter Adapter to inspect.
 * @return True while another input event must not be consumed.
 */
bool boxrig_x11_input_adapter_blocked(
    const boxrig_x11_input_adapter *adapter);

/**
 * Releases XIM, selection, and pending-input resources.
 *
 * @param adapter Adapter to destroy, or NULL.
 */
void boxrig_x11_input_adapter_destroy(boxrig_x11_input_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
