/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_INPUT_H
#define BOXRIG_WIN32_INPUT_H

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/editable.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/view.h"

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Win32-to-logical-input adapter used by the Win32 sample. */
typedef struct boxrig_win32_input_adapter boxrig_win32_input_adapter;

/** Result of synchronizing native IMM composition and candidate placement. */
typedef enum boxrig_win32_ime_placement_status {
    /** Internal state or coordinates could not be synchronized safely. */
    BOXRIG_WIN32_IME_PLACEMENT_ERROR = -1,
    /** No request was needed, or every required IMM request was accepted. */
    BOXRIG_WIN32_IME_PLACEMENT_OK = 0,
    /** Only ImmSetCompositionWindow() rejected its requested form. */
    BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED,
    /** Only ImmSetCandidateWindow() rejected its requested form. */
    BOXRIG_WIN32_IME_PLACEMENT_CANDIDATE_REJECTED,
    /** Both native IMM placement requests were rejected. */
    BOXRIG_WIN32_IME_PLACEMENT_BOTH_REJECTED
} boxrig_win32_ime_placement_status;

/**
 * Creates a Win32 logical-input adapter.
 *
 * @param window Terminal view window and clipboard owner.
 * @param terminal Terminal that receives logical input events.
 * @param cell_width Positive terminal cell width in pixels.
 * @param cell_height Positive terminal cell height in pixels.
 * @param paste_capacity Maximum accepted clipboard payload in bytes.
 * @param paste_chunk_size Preferred bytes per complete UTF-8 paste chunk (at least one scalar).
 * @param adapter_out Receives the new adapter.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_input_adapter_create(
    HWND window,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t paste_capacity,
    size_t paste_chunk_size,
    boxrig_win32_input_adapter **adapter_out);

/**
 * Attaches a cached application-owned editable document to the adapter.
 *
 * @param adapter Adapter to configure.
 * @param document Borrowed view-owned endpoint, or NULL to detach it.
 * @return Zero on success, or -1 for invalid input or an active transaction.
 *
 * @remarks The endpoint must outlive the adapter while attached. Native IME
 * requests read only its cached snapshot and never call the application.
 */
int boxrig_win32_input_adapter_set_editable_document(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_editable_document *document);

/**
 * Requests input inhibition or resumption on the owning event thread.
 * @param adapter Frontend whose terminal input is controlled.
 * @param enabled True to resume, false to cancel unsubmitted input.
 * @return OK when complete, BUSY while a paste terminator awaits capacity or
 * during an input callback, INVALID_ARGUMENT, LIMIT_EXCEEDED, or UNAVAILABLE.
 * @remarks A disable request accepted as STOPPING advances the generation and
 * immediately rejects new terminal input. retry() completes the required paste
 * terminator. DISABLED means it entered core, not that transport output drained.
 * Reentrant BUSY preserves the old state. Local routing, selection, copy, and
 * terminal protocol replies remain available. Held keys stay inhibited through
 * release. The borrowed IMM context is detached until resume or destruction;
 * the host must keep it alive and leave its association unchanged meanwhile.
 * Caller-retained native messages and direct composition callbacks must be
 * discarded when their saved generation differs from get_input_state().
 */
boxrig_ui_status boxrig_win32_input_adapter_set_input_enabled(
    boxrig_win32_input_adapter *adapter, bool enabled);

/**
 * Reads the input phase and generation without advancing pending work.
 * @param adapter Adapter on its owning event thread.
 * @param state_out Required state output.
 * @return OK or INVALID_ARGUMENT.
 */
boxrig_ui_status boxrig_win32_input_adapter_get_input_state(
    const boxrig_win32_input_adapter *adapter, boxrig_ui_input_state *state_out);

/**
 * Exchanges input settings when no native key or IME transaction is pending.
 * @param adapter Frontend to configure on its owning event thread.
 * @param config Complete replacement, copied using the core validation rules.
 * @return OK, BUSY while a key is held or input/IME/clipboard/paste is pending,
 *         INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks BUSY and other failures preserve all old settings and input state.
 */
boxrig_ui_status boxrig_win32_input_adapter_set_input_config(
    boxrig_win32_input_adapter *adapter, const boxrig_input_config *config);

/**
 * Installs an application key router used before terminal input encoding.
 *
 * @param adapter Adapter to configure.
 * @param router Router callback, or NULL to disable application routing.
 * @param context Opaque callback context, which must be NULL without a router.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_win32_input_adapter_set_key_router(
    boxrig_win32_input_adapter *adapter,
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
int boxrig_win32_input_adapter_set_mouse_router(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_view *view,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Reports copy, paste, and current reconversion capabilities.
 *
 * @param adapter Adapter to inspect.
 * @return A complete portable capability snapshot, or no capabilities for
 * NULL.
 */
boxrig_ui_view_action_capabilities
boxrig_win32_input_adapter_view_action_capabilities(
    const boxrig_win32_input_adapter *adapter);

/**
 * Starts one shortcut-independent portable view action.
 *
 * @param adapter Adapter that owns the native action transaction.
 * @param view View providing an immutable copy snapshot. This may be NULL for
 * paste and reconvert but is required for copy.
 * @param action Copy or paste with exactly one target, or reconvert with zero.
 * @return A portable UI status. A second pending paste returns BOXRIG_UI_BUSY.
 *
 * @remarks Reconversion reads only the attached cached editable snapshot and
 * starts one transaction before calling the native IME. Copy validates UTF-8,
 * converts LF to CRLF in NUL-terminated
 * CF_UNICODETEXT, and completes movable global storage before OpenClipboard
 * and EmptyClipboard. Failure before EmptyClipboard preserves the previous
 * clipboard. If EmptyClipboard succeeds and SetClipboardData fails, Win32 has
 * no rollback for the old formats. After SetClipboardData succeeds the system
 * owns the storage; CloseClipboard failure can still make this function return
 * BOXRIG_UI_UNAVAILABLE.
 */
boxrig_ui_status boxrig_win32_input_adapter_apply_view_action(
    boxrig_win32_input_adapter *adapter,
    boxrig_ui_view *view,
    const boxrig_ui_view_action *action);

/**
 * Updates the cell dimensions used for pointer hit testing.
 *
 * @param adapter Adapter to configure.
 * @param cell_width Positive terminal cell width in pixels.
 * @param cell_height Positive terminal cell height in pixels.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_win32_input_adapter_set_cell_size(
    boxrig_win32_input_adapter *adapter,
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
 * @remarks Pointer coordinates subtract this origin. IMM composition and
 * candidate placement add it to component-local coordinates.
 */
int boxrig_win32_input_adapter_set_viewport_origin(
    boxrig_win32_input_adapter *adapter,
    int x,
    int y);

/**
 * Returns the frontend-owned composition associated with this adapter.
 *
 * @param adapter Adapter to inspect.
 * @return Borrowed composition, or NULL for invalid input.
 */
boxrig_composition *boxrig_win32_input_adapter_composition(
    boxrig_win32_input_adapter *adapter);

/**
 * Starts one IMM preedit transaction.
 *
 * @param adapter Adapter receiving the transition.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_input_adapter_composition_start(
    boxrig_win32_input_adapter *adapter);

/**
 * Applies one UTF-16 IMM composition snapshot.
 *
 * @param adapter Adapter receiving the transition.
 * @param text UTF-16 composition units.
 * @param text_length Number of UTF-16 units in text.
 * @param attributes Optional IMM attribute for each UTF-16 unit.
 * @param attribute_count Number of bytes in attributes.
 * @param clauses Optional ordered UTF-16 clause boundaries.
 * @param clause_count Number of DWORD boundaries in clauses.
 * @param caret UTF-16 unit position of the preedit caret.
 * @return Zero on success, or -1 on invalid or rejected input.
 */
int boxrig_win32_input_adapter_composition_update_utf16(
    boxrig_win32_input_adapter *adapter,
    const WCHAR *text,
    size_t text_length,
    const BYTE *attributes,
    size_t attribute_count,
    const DWORD *clauses,
    size_t clause_count,
    DWORD caret);

/**
 * Completes and removes the current transient IMM preedit.
 *
 * @param adapter Adapter receiving the transition.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_input_adapter_composition_complete(
    boxrig_win32_input_adapter *adapter);

/**
 * Cancels and removes the current transient IMM preedit.
 *
 * @param adapter Adapter receiving the transition.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_input_adapter_composition_cancel(
    boxrig_win32_input_adapter *adapter);

/**
 * Commits one UTF-16 IMM result atomically.
 *
 * @param adapter Adapter receiving committed text.
 * @param text UTF-16 result units.
 * @param text_length Number of UTF-16 units in text.
 * @return Zero when accepted, including while awaiting output capacity, or
 * -1 on invalid input or failure.
 */
int boxrig_win32_input_adapter_composition_commit_utf16(
    boxrig_win32_input_adapter *adapter,
    const WCHAR *text,
    size_t text_length);

/**
 * Updates native composition and candidate windows from the placed caret.
 *
 * @param adapter Adapter to synchronize after composition rendering.
 * @return A placement status distinguishing accepted, rejected, and invalid
 * synchronization.
 *
 * @remarks A non-error rejection is display degradation only. IMM acceptance
 * does not guarantee the native popup's actual pixel position.
 */
boxrig_win32_ime_placement_status
boxrig_win32_input_adapter_synchronize_candidate(
    boxrig_win32_input_adapter *adapter);

/**
 * Reissues native composition and candidate placement after a screen move.
 *
 * @param adapter Adapter to synchronize from its retained placement.
 * @return A placement status distinguishing accepted, rejected, and invalid
 * synchronization.
 *
 * @remarks Call after the input window or one of its ancestors changes screen
 * position without changing the candidate's client coordinates. A visible
 * candidate reissues both accepted forms once; an absent candidate or input
 * context is a normal no-op. IMM acceptance does not guarantee the native
 * popup's actual pixel position.
 */
boxrig_win32_ime_placement_status
boxrig_win32_input_adapter_resynchronize_candidate(
    boxrig_win32_input_adapter *adapter);

/**
 * Processes an IME-owned key before the host calls TranslateMessage().
 *
 * @param adapter Adapter to run.
 * @param message Message removed from the current thread's queue.
 * @return One when consumed, zero when the host must continue normal message
 * translation and dispatch, or -1 on failure.
 *
 * @remarks This entry reads VK_PROCESSKEY through ImmGetVirtualKey(), whose
 * result is only available before TranslateMessage(). It consumes only an
 * Escape that cancels an active application-owned reconversion transaction.
 */
int boxrig_win32_input_adapter_pretranslate_message(
    boxrig_win32_input_adapter *adapter,
    const MSG *message);

/**
 * Converts one Win32 window message to logical terminal input.
 *
 * @param adapter Adapter to run.
 * @param message Window message identifier.
 * @param word_parameter Message WPARAM.
 * @param long_parameter Message LPARAM.
 * @param result_out Receives the handled message result.
 * @return One when handled, zero when unrelated or blocked, or -1 on failure.
 */
int boxrig_win32_input_adapter_process_message(
    boxrig_win32_input_adapter *adapter,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    LRESULT *result_out);

/**
 * Queues normalized UTF-8 as one streamed paste transaction.
 *
 * @param adapter Adapter to run.
 * @param bytes Untrusted length-delimited clipboard bytes.
 * @param length Number of bytes in bytes.
 * @return Zero when accepted, or -1 for invalid input or failure.
 */
int boxrig_win32_input_adapter_queue_paste(
    boxrig_win32_input_adapter *adapter,
    const uint8_t *bytes,
    size_t length);

/**
 * Retries the currently blocked logical event or paste phase.
 *
 * @param adapter Adapter to run.
 * @return Zero while pending or after completion, or -1 on failure.
 */
int boxrig_win32_input_adapter_retry(boxrig_win32_input_adapter *adapter);

/**
 * Reports whether a logical event is waiting for terminal output capacity.
 *
 * @param adapter Adapter to inspect.
 * @return True while another input message must not be consumed.
 */
bool boxrig_win32_input_adapter_blocked(
    const boxrig_win32_input_adapter *adapter);

/**
 * Releases clipboard, text, and pending-input resources.
 *
 * @param adapter Adapter to destroy, or NULL.
 */
void boxrig_win32_input_adapter_destroy(boxrig_win32_input_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
