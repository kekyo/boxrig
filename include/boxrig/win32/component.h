/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_COMPONENT_H
#define BOXRIG_WIN32_COMPONENT_H

#include "boxrig/ui/component.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/scrollbar.h"
#include "boxrig/ui/view.h"
#include "boxrig/win32/input.h"

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque terminal component using one explicit native hosting mode. */
typedef struct boxrig_win32_terminal_component
    boxrig_win32_terminal_component;

/**
 * Application-owned presenter for a component's retained Win32 surface.
 *
 * @param context Callback context supplied during registration.
 * @param surface Borrowed memory DC containing the current component frame.
 * @param size Exact surface size in pixels.
 * @param dirty Bounded nonempty rectangle updated by this frame.
 * @return BOXRIG_UI_OK when the application accepted the pixels, otherwise a
 * portable UI status that makes the component frame fail.
 *
 * @remarks Surface, size, and dirty are valid only for the callback duration
 * and use component-surface coordinates. The caller places that surface at
 * the allocation origin in its target. The callback may copy or present pixels
 * but must not retain native objects or call component APIs reentrantly. It
 * lets an explicit host integration present through a layered toplevel without
 * giving the component ownership of that toplevel.
 */
typedef boxrig_ui_status (*boxrig_win32_component_present_callback)(
    void *context,
    HDC surface,
    SIZE size,
    const RECT *dirty);

/**
 * Caller-owned Win32 targets fixed for a borrowed component lifetime.
 *
 * Both HWNDs and the presenter context must outlive the component. The
 * component never owns either HWND's geometry, style, procedure, visibility,
 * or lifetime.
 */
typedef struct boxrig_win32_terminal_borrowed_target {
    /** Caller-owned HWND associated with final component presentation. */
    HWND presentation_window;
    /** Caller-owned HWND receiving forwarded input and owning IMM state. */
    HWND input_window;
    /** Required callback controlling final presentation. */
    boxrig_win32_component_present_callback presenter;
    /** Opaque context passed to presenter. */
    void *presenter_context;
} boxrig_win32_terminal_borrowed_target;

/** Bounded creation configuration for one Win32 terminal child control. */
typedef struct boxrig_win32_terminal_component_config {
    /** Portable retained-view limits. */
    boxrig_ui_view_config view;
    /** Canonical layout authority or presentation-only attachment. */
    boxrig_ui_component_attachment attachment;
    /**
     * Visual scrollbar implementation selected for this component.
     *
     * Borrowed-target creation accepts NONE or COMPOSITED, not NATIVE_CHILD.
     */
    boxrig_ui_component_scrollbar_presentation scrollbar_presentation;
    /** Maximum accepted native clipboard payload. The default is 8 MiB. */
    size_t paste_capacity;
    /** Preferred paste bytes, default 4 KiB; a UTF-8 scalar is never split. */
    size_t paste_chunk_size;
    /** Initial component-surface transparency request. */
    boxrig_ui_transparency_config transparency;
} boxrig_win32_terminal_component_config;

/**
 * Returns deterministic Win32 component defaults.
 *
 * @return Layout-authority attachment with a native scrollbar, 8 MiB paste,
 * and 4 KiB paste chunks.
 */
boxrig_win32_terminal_component_config
boxrig_win32_terminal_component_config_default(void);

/**
 * Creates a terminal child control inside a caller-owned parent.
 *
 * @param instance Module instance used to register the private control class.
 * @param parent Caller-owned same-thread parent that outlives the component.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Bounded component configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE for authority or metric conflicts, otherwise
 * a portable UI status.
 *
 * @remarks The component borrows instance, parent, model, and terminal. It
 * owns only its child HWND, renderer, input/IMM adapter, scrollbar, and view.
 * It never starts a message loop, creates a session or process, posts WM_QUIT,
 * activates the toplevel, or destroys the parent.
 * Opaque rendering requires Windows 7 or Vista SP2 with the Platform Update.
 * The component advertises alpha target support only when its renderer can
 * provide it; preferred transparency otherwise resolves to opaque output.
 */
boxrig_ui_status boxrig_win32_terminal_component_create_child(
    HINSTANCE instance,
    HWND parent,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out);

/**
 * Creates a terminal component on caller-owned Win32 targets.
 *
 * @param instance Module instance used by renderer resources.
 * @param target Caller-owned presentation, input, and presenter targets.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Bounded component configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE for incompatible targets or metrics,
 * otherwise a portable UI status.
 *
 * @remarks Both HWNDs must belong to the creating process and current UI
 * thread. The component never moves, shows, styles, subclasses, or destroys
 * either HWND. The caller forwards input, focus, pointer, clipboard, and IMM
 * messages from its WndProc, supplies confirmed target and viewport dimensions,
 * and presents completed frames through the callback. Multiple components
 * sharing a target require caller-side focus and allocation hit-test routing.
 */
boxrig_ui_status boxrig_win32_terminal_component_create_on_target(
    HINSTANCE instance,
    const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out);

/**
 * Stops new message and frame delivery while retaining owned resources.
 *
 * @param component Component to quiesce.
 * @return A portable UI status. Repeated calls are harmless.
 */
boxrig_ui_status boxrig_win32_terminal_component_quiesce(
    boxrig_win32_terminal_component *component);

/**
 * Releases component-owned resources without touching borrowed objects.
 *
 * @param component Component to release, or NULL.
 *
 * @remarks Destroy the component before its model, parent, borrowed HWNDs, or
 * presenter context. This function never destroys a caller-owned HWND or posts
 * a thread-quit message.
 */
void boxrig_win32_terminal_component_destroy(
    boxrig_win32_terminal_component *component);

/**
 * Returns the selected native hosting mode.
 *
 * @param component Component to inspect.
 * @return Creation-time mode, or BOXRIG_UI_COMPONENT_OWNED_CHILD for NULL.
 */
boxrig_ui_component_native_hosting
boxrig_win32_terminal_component_native_hosting(
    const boxrig_win32_terminal_component *component);

/**
 * Returns the component-owned visible child HWND.
 *
 * @param component Component to inspect.
 * @return Owned child, or NULL for borrowed-target and detached components.
 */
HWND boxrig_win32_terminal_component_owned_window(
    const boxrig_win32_terminal_component *component);

/**
 * Returns the active native presentation HWND.
 *
 * @param component Component to inspect.
 * @return Owned child or caller-owned target, or NULL.
 */
HWND boxrig_win32_terminal_component_presentation_window(
    const boxrig_win32_terminal_component *component);

/**
 * Returns the active native input HWND.
 *
 * @param component Component to inspect.
 * @return Owned child or caller-owned target, or NULL.
 */
HWND boxrig_win32_terminal_component_input_window(
    const boxrig_win32_terminal_component *component);

/**
 * Reports whether the component owns its active input HWND.
 *
 * @param component Component to inspect.
 * @return True only for a live owned-child component.
 */
bool boxrig_win32_terminal_component_input_window_owned(
    const boxrig_win32_terminal_component *component);

/**
 * Returns the optional component-owned standard scrollbar HWND.
 *
 * @param component Component to inspect.
 * @return Child scrollbar handle, or NULL.
 */
HWND boxrig_win32_terminal_component_scrollbar_window(
    const boxrig_win32_terminal_component *component);

/**
 * Supplies the confirmed extent of the active presentation target.
 *
 * @param component Borrowed-target component to configure.
 * @param width Nonzero target width in pixels.
 * @param height Nonzero target height in pixels.
 * @return A portable UI status.
 *
 * @remarks This is valid only for borrowed-target hosting. It updates clipping
 * bounds only and never resizes the target. Call it after every confirmed
 * client-size change before allocating a viewport outside the old extent.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_target_extent(
    boxrig_win32_terminal_component *component,
    unsigned int width,
    unsigned int height);

/**
 * Returns the portable view owned by a component.
 *
 * @param component Component to inspect.
 * @return Borrowed view, or NULL.
 */
boxrig_ui_view *boxrig_win32_terminal_component_view(
    boxrig_win32_terminal_component *component);

/**
 * Returns the component-owned transient input-method composition.
 *
 * @param component Component to inspect.
 * @return Borrowed composition, or NULL.
 */
boxrig_composition *boxrig_win32_terminal_component_composition(
    boxrig_win32_terminal_component *component);

/**
 * Returns the caller-owned model borrowed by a component.
 *
 * @param component Component to inspect.
 * @return Borrowed model, or NULL.
 */
boxrig_ui_model *boxrig_win32_terminal_component_model(
    boxrig_win32_terminal_component *component);

/**
 * Enables or disables the application-owned editable-document provider.
 *
 * @param component Component whose view owns the cached endpoint.
 * @param available Whether native IME reconversion may use that endpoint.
 * @return BOXRIG_UI_BUSY during a reconversion transaction, otherwise a
 * portable UI status.
 *
 * @remarks Enabling only attaches the component view's callback-free cache.
 * The application publishes snapshots and pulls replacement requests through
 * boxrig_ui_view_editable_document(). Disabling never derives editable text
 * from the terminal screen, scrollback, selection, or outbound bytes.
 */
boxrig_ui_status
boxrig_win32_terminal_component_set_editable_document_provider(
    boxrig_win32_terminal_component *component,
    bool available);

/**
 * Applies one actual component viewport allocation.
 *
 * @param component Component to place.
 * @param allocation Actual native allocation and layout context.
 * @param layout_out Receives effective terminal grid and excess area.
 * @return A portable UI status.
 *
 * @remarks For owned-child hosting, x and y are parent-relative and only
 * component-owned children are moved. For borrowed-target hosting, x and y are
 * the caller target origin; the viewport must fit the last target extent and
 * no HWND is moved. Width includes the selected scrollbar exactly once. Grid
 * changes remain revisioned notifications for application-owned terminal and
 * transport updates. Allocation never creates an application size command or
 * resizes an application-owned HWND.
 */
boxrig_ui_status boxrig_win32_terminal_component_allocate(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out);

/**
 * Reads the last effective allocation and grid.
 *
 * @param component Component to inspect.
 * @param layout_out Receives the current layout.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_layout(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_layout_result *layout_out);

/**
 * Changes the normal-state grid anchor without resizing native objects.
 *
 * @param component Current model layout authority.
 * @param rows Preferred nonzero rows.
 * @param columns Preferred nonzero columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_preferred_grid(
    boxrig_win32_terminal_component *component,
    size_t rows,
    size_t columns);

/**
 * Reads the normal-state preferred grid.
 *
 * @param component Component to inspect.
 * @param rows_out Receives preferred rows.
 * @param columns_out Receives preferred columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_preferred_grid(
    const boxrig_win32_terminal_component *component,
    size_t *rows_out,
    size_t *columns_out);

/**
 * Reads the current application size-command lifecycle.
 *
 * @param component Component to inspect.
 * @param state_out Receives IDLE, READY, or DISPATCHED.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_size_command_state(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_size_command_state *state_out);

/**
 * Takes the one currently executable component layout action.
 *
 * @param component Current layout-authority component.
 * @param action_out Receives a tagged content-size or grid-resize action.
 * @return True only when one READY action advances to DISPATCHED.
 *
 * @remarks Content-size has priority over grid resize. Complete the returned
 * action with the kind-specific completion operation before taking another.
 */
bool boxrig_win32_terminal_component_take_layout_action(
    boxrig_win32_terminal_component *component,
    boxrig_ui_layout_action *action_out);

/**
 * Completes one application size command with the host's actual allocation.
 *
 * @param component Component containing the dispatched command.
 * @param revision Echoed command revision.
 * @param outcome Accepted, constrained, rejected, or superseded result.
 * @param allocation_width Actual content width in 26.6 output pixels.
 * @param allocation_height Actual content height in 26.6 output pixels.
 * @param applied_out Receives false for an obsolete revision.
 * @param layout_out Receives resulting effective layout.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_complete_size_command(
    boxrig_win32_terminal_component *component,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out);

/**
 * Records the application-owned result of one dispatched grid resize.
 *
 * @param component Component that returned the action.
 * @param revision Revision returned by take_layout_action.
 * @param succeeded Whether transport and terminal resize both succeeded.
 * @return BOXRIG_UI_OK on completion, BOXRIG_UI_STALE for an unknown or
 * completed revision, or BOXRIG_UI_INVALID_ARGUMENT for invalid state or a
 * successful completion whose terminal grid does not match the action.
 *
 * @remarks Failure quiesces the component because an external transport
 * resize cannot be rolled back safely.
 */
boxrig_ui_status boxrig_win32_terminal_component_complete_grid_resize(
    boxrig_win32_terminal_component *component,
    uint64_t revision,
    bool succeeded);

/**
 * Transactionally changes the copied raw font specification.
 *
 * @param component Component whose renderer, input, and layout change.
 * @param specification Candidate specification copied on success.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_font_specification(
    boxrig_win32_terminal_component *component,
    const boxrig_font_specification *specification);

/**
 * Reads the current raw font specification.
 *
 * @param component Component to inspect.
 * @param specification_out Receives a descriptor borrowing component storage.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_font_specification(
    const boxrig_win32_terminal_component *component,
    boxrig_font_specification *specification_out);

/**
 * Transactionally changes output DPI and rasterization policy.
 *
 * @param component Component to configure.
 * @param policy Candidate policy copied on success.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_font_rendering_policy(
    boxrig_win32_terminal_component *component,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the committed output font policy.
 *
 * @param component Component to inspect.
 * @param policy_out Receives the current policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_font_rendering_policy(
    const boxrig_win32_terminal_component *component,
    boxrig_font_rendering_policy *policy_out);

/**
 * Reads committed effective font metrics.
 *
 * @param component Component to inspect.
 * @param metrics_out Receives resolved metrics and revision.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status
boxrig_win32_terminal_component_get_effective_metrics(
    const boxrig_win32_terminal_component *component,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Registers one validated application font file without changing layout.
 *
 * @param component Component owning the private font catalog.
 * @param path Length-delimited UTF-8 file path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based font face index.
 * @param resource_id_out Receives a nonzero exact resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status
boxrig_win32_terminal_component_register_font_file(
    boxrig_win32_terminal_component *component,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Registers copied immutable application font bytes without changing layout.
 *
 * @param component Component owning the private font catalog.
 * @param data Font bytes copied before this function returns.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based font face index.
 * @param resource_id_out Receives a nonzero exact resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status
boxrig_win32_terminal_component_register_font_memory(
    boxrig_win32_terminal_component *component,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Returns the active exact application font resource.
 *
 * @param component Component to inspect.
 * @return Resource identifier, or BOXRIG_FONT_RESOURCE_NONE.
 */
boxrig_font_resource_id
boxrig_win32_terminal_component_primary_font_resource(
    const boxrig_win32_terminal_component *component);

/**
 * Returns the revision of this component's private font catalog.
 *
 * @param component Component to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_win32_terminal_component_font_catalog_revision(
    const boxrig_win32_terminal_component *component);

/**
 * Changes explicit extra line height without changing raw font size.
 *
 * @param component Component to configure transactionally.
 * @param adjustment_dip Nonnegative logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_win32_terminal_component_set_line_height_adjustment(
    boxrig_win32_terminal_component *component,
    float adjustment_dip);

/**
 * Reads the explicit raw line-height addition.
 *
 * @param component Component to inspect.
 * @param adjustment_dip_out Receives the logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_win32_terminal_component_get_line_height_adjustment(
    const boxrig_win32_terminal_component *component,
    float *adjustment_dip_out);

/**
 * Applies one lazily resolved deterministic visual zoom step.
 *
 * @param component Component to configure transactionally.
 * @param direction Increase or decrease direction.
 * @param policy Candidate range and increment.
 * @param changed_out Receives true only when the desired visual target changes.
 * @return A portable UI status.
 *
 * @remarks Visual zoom does not mutate the queried raw font specification. One
 * presentation generation remains in flight until a frame commits it. Calls in
 * that interval replace one latest pending target instead of replacing the
 * in-flight size command or retaining every intermediate step.
 */
boxrig_ui_status boxrig_win32_terminal_component_visual_zoom(
    boxrig_win32_terminal_component *component,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out);

/**
 * Resets the current visual zoom sequence to its starting metrics.
 *
 * @param component Component to configure transactionally.
 * @param changed_out Receives true only when the desired visual target changes.
 * @return A portable UI status.
 *
 * @remarks A reset received during an in-flight generation becomes its latest
 * pending target and is promoted only after that generation is presented.
 */
boxrig_ui_status boxrig_win32_terminal_component_visual_zoom_reset(
    boxrig_win32_terminal_component *component,
    bool *changed_out);

/**
 * Selects automatic, forced LTR, or forced RTL row layout.
 *
 * @param component Component to configure.
 * @param policy New row direction policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_text_direction(
    boxrig_win32_terminal_component *component,
    boxrig_text_direction_policy policy);

/**
 * Returns the current row direction policy.
 *
 * @param component Component to inspect.
 * @return Current policy, or BOXRIG_TEXT_DIRECTION_AUTO for NULL.
 */
boxrig_text_direction_policy
boxrig_win32_terminal_component_text_direction(
    const boxrig_win32_terminal_component *component);

/**
 * Installs a clean-to-dirty application-loop frame notification.
 *
 * @param component Component to configure.
 * @param callback Clean-to-dirty callback, or NULL.
 * @param context Callback context, which must be NULL without callback.
 * @return A portable UI status.
 *
 * @remarks The callback only requests later dispatch and never renders or
 * waits reentrantly.
 */
boxrig_ui_status
boxrig_win32_terminal_component_set_frame_request_callback(
    boxrig_win32_terminal_component *component,
    boxrig_ui_frame_request_callback callback,
    void *context);

/**
 * Changes whether component frames may be presented.
 *
 * @param component Component to configure.
 * @param visible Whether the view may produce visible presentation.
 * @return A portable UI status.
 *
 * @remarks Owned-child hosting shows or hides component children.
 * Borrowed-target hosting changes only portable presentation permission and
 * never calls ShowWindow() for either caller-owned HWND.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_visible(
    boxrig_win32_terminal_component *component,
    bool visible);

/**
 * Reports the application-supplied component visibility state.
 *
 * @param component Component to inspect.
 * @return True only while configured visible.
 */
bool boxrig_win32_terminal_component_visible(
    const boxrig_win32_terminal_component *component);

/**
 * Requests focus without raising or activating the toplevel.
 *
 * @param component Visible component to focus.
 * @return A portable UI status.
 *
 * @remarks Owned-child hosting focuses its child. Borrowed-target hosting calls
 * SetFocus() only for the creation-time input HWND; the caller remains
 * responsible for activation and routing focus messages.
 */
boxrig_ui_status boxrig_win32_terminal_component_request_focus(
    boxrig_win32_terminal_component *component);

/**
 * Reports the most recently dispatched native focus state.
 *
 * @param component Component to inspect.
 * @return True after WM_SETFOCUS and before the corresponding WM_KILLFOCUS.
 */
bool boxrig_win32_terminal_component_focused(
    const boxrig_win32_terminal_component *component);

/**
 * Requests input inhibition or resumption for this component.
 * @param component Component on its owning event thread.
 * @param enabled True to resume, false to cancel unsubmitted input.
 * @return OK when complete, BUSY while a paste terminator awaits capacity or
 * during input dispatch, INVALID_ARGUMENT, LIMIT_EXCEEDED, or UNAVAILABLE.
 * @remarks An accepted disable request immediately advances the generation.
 * frame() retries a required paste terminator; get_input_state() distinguishes
 * STOPPING from DISABLED. Reentrant BUSY leaves the old state unchanged.
 * Selection, copy, local routes, rendering, and terminal replies continue.
 * Already submitted core/transport output remains queued. See the adapter's
 * set_input_enabled() contract for retained messages and borrowed IMM contexts.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_input_enabled(
    boxrig_win32_terminal_component *component, bool enabled);

/**
 * Reads the input phase and generation without advancing pending work.
 * @param component Component on its owning event thread.
 * @param state_out Required state output.
 * @return OK or INVALID_ARGUMENT.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_input_state(
    const boxrig_win32_terminal_component *component, boxrig_ui_input_state *state_out);

/**
 * Exchanges input settings when no native key or IME transaction is pending.
 * @param component Frontend to configure on its owning event thread.
 * @param config Complete replacement, copied using the core validation rules.
 * @return OK, BUSY while a key is held or input/IME/clipboard/paste is pending,
 *         INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks BUSY and other failures preserve all old settings and input state.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_input_config(
    boxrig_win32_terminal_component *component, const boxrig_input_config *config);

/**
 * Installs application shortcut routing ahead of terminal encoding.
 *
 * @param component Component to configure.
 * @param router Router callback, or NULL.
 * @param context Callback context, which must be NULL without router.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_key_router router,
    void *context);

/**
 * Installs application pointer routing ahead of terminal mouse encoding.
 *
 * @param component Component to configure.
 * @param router Router callback, or NULL.
 * @param context Callback context, which must be NULL without router.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_mouse_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Reports supported copy, paste, and current reconversion capabilities.
 *
 * @param component Component to inspect.
 * @return A complete capability snapshot, or no capabilities for NULL.
 */
boxrig_ui_view_action_capabilities
boxrig_win32_terminal_component_view_action_capabilities(
    const boxrig_win32_terminal_component *component);

/**
 * Starts one shortcut-independent portable view action.
 *
 * @param component Component that owns the view and native transaction.
 * @param action Copy or paste with exactly one target, or reconvert with zero.
 * @return A portable UI status.
 *
 * @remarks Reconversion uses only the explicitly enabled cached editable
 * endpoint and never derives text from the terminal. Copy prepares validated
 * CF_UNICODETEXT storage before opening or
 * emptying the clipboard. If EmptyClipboard succeeds but SetClipboardData
 * fails, the previous system clipboard cannot be restored. A close failure
 * can return BOXRIG_UI_UNAVAILABLE after ownership was transferred.
 */
boxrig_ui_status boxrig_win32_terminal_component_apply_view_action(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_view_action *action);

/**
 * Queues one bounded user-originated UTF-8 paste transaction.
 *
 * @param component Component receiving clipboard bytes.
 * @param bytes Untrusted length-delimited UTF-8 bytes.
 * @param length Number of bytes in bytes.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_queue_paste(
    boxrig_win32_terminal_component *component,
    const uint8_t *bytes,
    size_t length);

/**
 * Maps a component-local 26.6 point through retained shaped row layout.
 *
 * @param component Component to inspect.
 * @param x Horizontal 26.6 output-pixel position.
 * @param y Vertical 26.6 output-pixel position.
 * @param result_out Receives grid and logical-cell coordinates.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_hit_test(
    boxrig_win32_terminal_component *component,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out);

/**
 * Reads the shared scrollbar state for an application-owned external bar.
 * @param component Component to inspect.
 * @param state_out Receives the last synchronized values and shared revision.
 * @return A portable UI status.
 * @remarks Synchronize the UI model after direct terminal mutations. Read this
 * state in the scheduled frame and update the external bar only when its
 * revision changes. The getter neither moves the viewport nor emits callbacks.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_scrollbar_state(
    boxrig_win32_terminal_component *component, boxrig_scrollbar_state *state_out);

/**
 * Requests a standard scrollbar visibility policy without changing history.
 * @param component Component to change.
 * @param policy ALWAYS (default), AUTO while history is navigable, or NEVER.
 * @return A portable status; BUSY leaves the request unchanged during a layout transaction.
 * @remarks Continue frame and layout-action dispatch. A required PTY/grid resize
 * must complete before the bar and content width commit together. A failed
 * grid resize follows the component's existing quiesce contract. NONE remains
 * external: no standard bar or width is introduced by this function.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_scrollbar_visibility(
    boxrig_win32_terminal_component *component, boxrig_ui_scrollbar_visibility policy);

/**
 * Reads requested policy and committed standard-bar geometry.
 * @param component Component to inspect.
 * @param state_out Receives visibility, native-pixel width and pending state.
 * @return A portable status. This read does not dispatch or alter layout.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_scrollbar_visibility(
    const boxrig_win32_terminal_component *component, boxrig_ui_scrollbar_visibility_state *state_out);

/**
 * Reads the terminal viewport as a component-local scrollbar model.
 *
 * @param component Component to inspect.
 * @param model_out Receives the portable scrollbar model.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_scrollbar_model(
    boxrig_win32_terminal_component *component,
    boxrig_scrollbar_model *model_out);

/**
 * Applies one portable scrollbar operation to the borrowed terminal.
 *
 * @param component Component whose viewport is controlled.
 * @param action Portable line, page, edge, or position operation.
 * @param position Absolute position used by BOXRIG_SCROLLBAR_POSITION.
 * @param changed_out Receives whether the viewport changed.
 * @return A portable UI status.
 * @remarks A changed position synchronizes the shared UI model and requests
 * frames. Repeated positions, absent history and alternate screens are no-ops.
 * changed_out remains true if a move is followed by a synchronization error.
 */
boxrig_ui_status boxrig_win32_terminal_component_apply_scrollbar_action(
    boxrig_win32_terminal_component *component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out);

/**
 * Offers one queued message before the host calls TranslateMessage().
 *
 * @param component Component receiving the message.
 * @param message Message removed from the current thread's queue.
 * @return Unrelated, handled, or failure.
 *
 * @remarks A handled message must not be translated or dispatched again.
 * This preserves the original virtual key behind VK_PROCESSKEY so Escape can
 * cancel application-owned IMM reconversion without reaching the terminal.
 */
boxrig_ui_component_event_result
boxrig_win32_terminal_component_pretranslate_message(
    boxrig_win32_terminal_component *component,
    const MSG *message);

/**
 * Dispatches one Win32 message without consuming parent or sibling messages.
 *
 * @param component Component receiving the message.
 * @param target HWND to which the message belongs.
 * @param message Message identifier.
 * @param word_parameter Message WPARAM.
 * @param long_parameter Message LPARAM.
 * @param result_out Receives the handled LRESULT.
 * @return Unrelated, handled, blocked, or failure.
 *
 * @remarks The private child WndProc calls this same entry. In borrowed-target
 * hosting the caller's WndProc forwards keyboard, focus, pointer, clipboard,
 * and IMM messages to the focused or allocation-hit component. Paint, size,
 * parent, and sibling messages remain caller-owned. A blocked result requires
 * retrying the same application-dispatched message after capacity is
 * available. This function never retrieves or waits for another message.
 */
boxrig_ui_component_event_result
boxrig_win32_terminal_component_dispatch_message(
    boxrig_win32_terminal_component *component,
    HWND target,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    LRESULT *result_out);

/**
 * Performs one bounded retained update, IMM placement, and scrollbar frame.
 *
 * @param component Component to dispatch after model synchronization.
 * @param result_out Receives remaining work and animation state.
 * @return A portable UI status.
 *
 * @remarks A completed zoom checkpoint can promote one latest target, so the
 * host must inspect the component size-command state after every successful
 * frame.
 */
boxrig_ui_status boxrig_win32_terminal_component_frame(
    boxrig_win32_terminal_component *component,
    boxrig_ui_component_frame_result *result_out);

/**
 * Returns a pending presentation event for the existing application wait loop.
 * @param component Component on its creation thread.
 * @return Borrowed handle, or NULL when presentation needs no event.
 * @remarks Re-query before each wait; resize/recreation may replace the handle.
 * After a wait consumes this event, call dispatch_activity before the next frame.
 * The application must not close or retain this handle across component calls.
 */
HANDLE boxrig_win32_terminal_component_activity_handle(
    boxrig_win32_terminal_component *component);
/**
 * Acknowledges that the application wait consumed the presentation event.
 * @param component Component whose current activity handle was signaled.
 * @return UI status; the next frame may submit one bounded batch.
 * @remarks Call only after a successful wait on activity_handle, on this thread.
 */
boxrig_ui_status boxrig_win32_terminal_component_dispatch_activity(
    boxrig_win32_terminal_component *component);

/**
 * Reissues retained IMM placement after the input HWND moves on screen.
 *
 * @param component Component whose native IME placement must be refreshed.
 * @return The nonfatal native placement result, or an error for an invalid or
 * inactive component.
 *
 * @remarks Call this after changing the screen position of the input HWND or
 * any ancestor, without waiting for a component frame. A host handling
 * WM_MOVE can call it after completing related native presentation updates. A
 * host that suppresses WM_MOVE by consuming WM_WINDOWPOSCHANGED must call it
 * from that equivalent screen-position notification. Candidate absence and a
 * missing input context are successful no-ops. The operation does not draw,
 * synchronize the model, request a frame, or invoke application callbacks.
 */
boxrig_win32_ime_placement_status
boxrig_win32_terminal_component_resynchronize_ime_placement(
    boxrig_win32_terminal_component *component);

/**
 * Returns the most recent nonfatal IMM placement result.
 *
 * @param component Component to inspect.
 * @return The last candidate-placement result, or an error for NULL.
 */
boxrig_win32_ime_placement_status
boxrig_win32_terminal_component_ime_placement_status(
    const boxrig_win32_terminal_component *component);

/**
 * Transactionally changes component-surface transparency only.
 *
 * @param component Component to configure.
 * @param config Requested transparency and explicit scope.
 * @return BOXRIG_UI_UNAVAILABLE without changing state for an unmet required
 * request, otherwise a portable UI status.
 *
 * @remarks Native-toplevel scope is never advertised or modified here.
 */
boxrig_ui_status boxrig_win32_terminal_component_set_transparency(
    boxrig_win32_terminal_component *component,
    const boxrig_ui_transparency_config *config);

/**
 * Reads requested, capable, and effective component transparency.
 *
 * @param component Component to inspect.
 * @param state_out Receives the resolved portable state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_transparency(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_transparency_state *state_out);

/**
 * Returns portable capabilities of this component instance.
 *
 * @param component Component to inspect.
 * @return Bitwise boxrig_ui_component_capability values, or zero for NULL.
 */
uint32_t boxrig_win32_terminal_component_capabilities(
    const boxrig_win32_terminal_component *component);

/**
 * Reads recoverable component state without logging or terminating.
 *
 * @param component Component to inspect.
 * @param diagnostic_out Receives current state and revisions.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_terminal_component_get_diagnostic(
    const boxrig_win32_terminal_component *component,
    boxrig_ui_component_diagnostic *diagnostic_out);

#ifdef __cplusplus
}
#endif

#endif
