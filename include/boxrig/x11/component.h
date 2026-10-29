/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_COMPONENT_H
#define BOXRIG_X11_COMPONENT_H

#include "boxrig/ui/component.h"
#include "boxrig/ui/animation.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/scrollbar.h"
#include "boxrig/ui/view.h"

#include <X11/Xlib.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque terminal component using one explicit native hosting mode. */
typedef struct boxrig_x11_terminal_component boxrig_x11_terminal_component;

/**
 * Caller-owned X11 targets fixed for a borrowed-target component lifetime.
 *
 * Both Windows must outlive the component. The component borrows their
 * identity without owning their geometry, event masks, mapping, or lifetime.
 */
typedef struct boxrig_x11_terminal_borrowed_target {
    /** InputOutput Window receiving final component pixels. */
    Window presentation_window;
    /** Window receiving forwarded input and owning XIM and selections. */
    Window input_window;
} boxrig_x11_terminal_borrowed_target;

/** Bounded creation configuration for one X11 terminal component. */
typedef struct boxrig_x11_terminal_component_config {
    /** Portable retained-view limits. */
    boxrig_ui_view_config view;
    /**
     * Borrowed shared runtime for this owning event loop. Required whenever
     * either initial blink policy is enabled; missing connection returns
     * UNAVAILABLE. The runtime must outlive the component.
     */
    boxrig_ui_animation_runtime *animation_runtime;
    /** Canonical layout authority or presentation-only attachment. */
    boxrig_ui_component_attachment attachment;
    /**
     * Visual scrollbar implementation selected for this component.
     *
     * Borrowed-target creation accepts NONE or COMPOSITED, not NATIVE_CHILD.
     */
    boxrig_ui_component_scrollbar_presentation scrollbar_presentation;
    /** Maximum accepted native selection payload. The default is 8 MiB. */
    size_t paste_capacity;
    /** Retryable terminal paste chunk. The default is 4 KiB. */
    size_t paste_chunk_size;
    /** Direct property or INCR chunk request. The default is 64 KiB. */
    size_t selection_transfer_chunk_size;
    /** Simultaneous outgoing INCR transfers. The default is 8. */
    size_t maximum_active_selection_transfers;
    /** Aggregate outgoing INCR payload. The default is 64 MiB. */
    size_t maximum_selection_transfer_bytes;
    /** Initial component-surface transparency request. */
    boxrig_ui_transparency_config transparency;
} boxrig_x11_terminal_component_config;

/**
 * Returns conservative X11 component defaults.
 *
 * @return Layout-authority attachment with a native scrollbar, 8 MiB paste,
 * 4 KiB paste chunks, 64 KiB selection chunks, eight outgoing INCR
 * transfers, and 64 MiB aggregate outgoing payload.
 */
boxrig_x11_terminal_component_config
boxrig_x11_terminal_component_config_default(void);

/**
 * Creates a child terminal surface inside a caller-owned parent Window.
 *
 * @param display Caller-owned connection that outlives the component.
 * @param parent Caller-owned parent that is never moved, mapped, or destroyed.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Bounded component configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE for an authority or metric conflict, otherwise
 * a portable UI status.
 *
 * @remarks The component borrows display, parent, model, and terminal. It owns
 * only its child Window, renderer, input method, scrollbar, and portable view.
 * It does not start an event loop or create a session or child process.
 */
boxrig_ui_status boxrig_x11_terminal_component_create_child(
    Display *display,
    Window parent,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out);

/**
 * Moves an owned child to another caller-owned parent on the same screen.
 *
 * @param component Running owned-child component on its owning event thread.
 * @param parent Live InputOutput parent on the component's Display and screen.
 * @return OK after the server processes the move; BUSY during input, rendering,
 * or layout transactions; INVALID_ARGUMENT for invalid arguments; UNAVAILABLE
 * for borrowed targets, incompatible parents or XIM preparation failure;
 * LIMIT_EXCEEDED if the input generation cannot advance.
 * @remarks Keeps the child Window, view, selection, settings and terminal.
 * Geometry and terminal dimensions stay unchanged; use allocate afterwards.
 * The parent cannot be the child or its descendant. Keep both parents alive
 * during the call; the old parent may be destroyed after success. Keep the new
 * parent alive until the next successful move or component destruction.
 * Pending keys, buttons, preedit and incoming paste must finish before retry.
 * Failures leave the old attachment intact. Connection loss is not recoverable
 * through this operation. Forward events as before, discard caller-retained
 * input from older input generations, and request focus after moving.
 * XIM is recreated because XNClientWindow cannot be changed after creation.
 * See https://www.x.org/releases/X11R7.6/doc/libX11/specs/libX11/libX11.html .
 */
boxrig_ui_status boxrig_x11_terminal_component_reparent(
    boxrig_x11_terminal_component *component, Window parent);

/**
 * Creates a terminal component on caller-owned X11 targets.
 *
 * @param display Caller-owned connection that outlives the component.
 * @param target Caller-owned presentation and input targets.
 * @param model Caller-owned model that outlives the component and its view.
 * @param config Bounded component configuration copied during creation.
 * @param component_out Receives the transactional component result.
 * @return BOXRIG_UI_UNAVAILABLE for incompatible native targets or metrics,
 * otherwise a portable UI status.
 *
 * @remarks The component does not map, move, resize, select events on, or
 * destroy either target. Both targets must be compatible InputOutput Windows
 * on the same screen with the same Visual, depth, and Colormap. The caller
 * selects events, forwards input, focus, XIM, selection, PropertyNotify,
 * DestroyNotify, and expose events, and supplies the confirmed
 * presentation-target extent and component viewport. Multiple components
 * sharing a target require caller-side focus and allocation hit-test routing.
 */
boxrig_ui_status boxrig_x11_terminal_component_create_on_target(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out);

/**
 * Stops new component event delivery while retaining owned resources.
 *
 * @param component Component to quiesce.
 * @return A portable UI status. Repeated calls are harmless.
 *
 * @remarks Quiescing relinquishes PRIMARY and CLIPBOARD ownership and cancels
 * every pending selection transfer.
 */
boxrig_ui_status boxrig_x11_terminal_component_quiesce(
    boxrig_x11_terminal_component *component);

/**
 * Releases component-owned resources without touching borrowed objects.
 *
 * @param component Component to release, or NULL.
 *
 * @remarks The caller destroys the component before its model, parent,
 * borrowed targets, or Display. This function never closes the Display or
 * destroys a caller-owned Window. Destroying the component relinquishes its
 * X11 selections; without an external clipboard manager their payloads do not
 * survive component or process destruction.
 */
void boxrig_x11_terminal_component_destroy(
    boxrig_x11_terminal_component *component);

/**
 * Returns the selected native hosting mode.
 *
 * @param component Component to inspect.
 * @return Creation-time hosting mode, or BOXRIG_UI_COMPONENT_OWNED_CHILD for
 * NULL.
 */
boxrig_ui_component_native_hosting
boxrig_x11_terminal_component_native_hosting(
    const boxrig_x11_terminal_component *component);

/**
 * Returns the component-owned visible child Window.
 *
 * @param component Component to inspect.
 * @return Owned child, or None for borrowed-target and detached components.
 */
Window boxrig_x11_terminal_component_owned_window(
    const boxrig_x11_terminal_component *component);

/**
 * Returns the active native presentation Window.
 *
 * @param component Component to inspect.
 * @return Owned child or caller-owned target, or None.
 */
Window boxrig_x11_terminal_component_presentation_window(
    const boxrig_x11_terminal_component *component);

/**
 * Returns the active native input Window.
 *
 * @param component Component to inspect.
 * @return Owned child or caller-owned target, or None.
 */
Window boxrig_x11_terminal_component_input_window(
    const boxrig_x11_terminal_component *component);

/**
 * Reports whether the component owns its active input Window.
 *
 * @param component Component to inspect.
 * @return True only for a live owned-child component.
 */
bool boxrig_x11_terminal_component_input_window_owned(
    const boxrig_x11_terminal_component *component);

/**
 * Returns the optional component-owned native scrollbar Window.
 *
 * @param component Component to inspect.
 * @return Native scrollbar child, or None.
 */
Window boxrig_x11_terminal_component_scrollbar_window(
    const boxrig_x11_terminal_component *component);

/**
 * Supplies the confirmed extent of the active presentation target.
 *
 * @param component Borrowed-target component to configure.
 * @param width Nonzero target width in pixels.
 * @param height Nonzero target height in pixels.
 * @return A portable UI status.
 *
 * @remarks This is valid only for borrowed-target hosting. It updates renderer
 * clipping only and never resizes the target. Call it after every confirmed
 * target resize before allocating or presenting a viewport outside the old
 * extent.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_target_extent(
    boxrig_x11_terminal_component *component,
    unsigned int width,
    unsigned int height);

/**
 * Returns the portable view owned by a component.
 *
 * @param component Component to inspect.
 * @return Borrowed view, or NULL.
 */
boxrig_ui_view *boxrig_x11_terminal_component_view(
    boxrig_x11_terminal_component *component);

/**
 * Returns the component-owned transient input-method composition.
 *
 * @param component Component to inspect.
 * @return Borrowed composition, or NULL.
 */
boxrig_composition *boxrig_x11_terminal_component_composition(
    boxrig_x11_terminal_component *component);

/**
 * Returns the caller-owned model borrowed by a component.
 *
 * @param component Component to inspect.
 * @return Borrowed model, or NULL.
 */
boxrig_ui_model *boxrig_x11_terminal_component_model(
    boxrig_x11_terminal_component *component);

/**
 * Applies one actual component viewport allocation.
 *
 * @param component Component to place.
 * @param allocation Actual native allocation and layout context.
 * @param layout_out Receives the effective terminal grid and excess area.
 * @return A portable UI status.
 *
 * @remarks For owned-child hosting, x and y are parent-relative and only the
 * component child is moved or resized. For borrowed-target hosting, x and y
 * are the presentation-target origin; the viewport must fit the last target
 * extent and no native target is moved. Width includes the selected scrollbar
 * exactly once. Grid changes remain revisioned notifications for application-
 * owned terminal and transport updates. Allocation never creates an
 * application size command or resizes a terminal, session, parent, or
 * toplevel.
 */
boxrig_ui_status boxrig_x11_terminal_component_allocate(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_component_allocation *allocation,
    boxrig_ui_layout_result *layout_out);

/**
 * Reads the last effective allocation and grid.
 *
 * @param component Component to inspect.
 * @param layout_out Receives the current effective layout.
 * @return BOXRIG_UI_INVALID_ARGUMENT before the first allocation, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_layout(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_layout_result *layout_out);

/**
 * Changes the normal-state grid anchor without resizing native objects.
 *
 * @param component Current model layout authority.
 * @param rows Preferred nonzero terminal rows.
 * @param columns Preferred nonzero terminal columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_preferred_grid(
    boxrig_x11_terminal_component *component,
    size_t rows,
    size_t columns);

/**
 * Reads the normal-state preferred terminal grid.
 *
 * @param component Component to inspect.
 * @param rows_out Receives preferred rows.
 * @param columns_out Receives preferred columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_preferred_grid(
    const boxrig_x11_terminal_component *component,
    size_t *rows_out,
    size_t *columns_out);

/**
 * Reads the current application size-command lifecycle.
 *
 * @param component Component to inspect.
 * @param state_out Receives IDLE, READY, or DISPATCHED.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_size_command_state(
    const boxrig_x11_terminal_component *component,
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
bool boxrig_x11_terminal_component_take_layout_action(
    boxrig_x11_terminal_component *component,
    boxrig_ui_layout_action *action_out);

/**
 * Completes one application size command using the host's actual allocation.
 *
 * @param component Component containing the dispatched command.
 * @param revision Command revision echoed by the host.
 * @param outcome Accepted, constrained, rejected, or superseded result.
 * @param allocation_width Actual content width in 26.6 output pixels.
 * @param allocation_height Actual content height in 26.6 output pixels.
 * @param applied_out Receives false for an obsolete revision.
 * @param layout_out Receives the resulting effective layout.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_complete_size_command(
    boxrig_x11_terminal_component *component,
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
boxrig_ui_status boxrig_x11_terminal_component_complete_grid_resize(
    boxrig_x11_terminal_component *component,
    uint64_t revision,
    bool succeeded);

/**
 * Transactionally changes the raw portable font specification.
 *
 * @param component Component whose renderer and input metrics change together.
 * @param specification Candidate specification copied on success.
 * @return BOXRIG_UI_UNAVAILABLE for a model metric conflict, otherwise a
 * portable UI status.
 *
 * @remarks Candidate font resolution and model geometry validation complete
 * before renderer, input, IME, and layout state commit. Failure preserves the
 * prior specification and effective metric revision.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_font_specification(
    boxrig_x11_terminal_component *component,
    const boxrig_font_specification *specification);

/**
 * Reads the current raw portable font specification.
 *
 * @param component Component to inspect.
 * @param specification_out Receives a descriptor borrowing component storage.
 * @return A portable UI status.
 *
 * @remarks Returned arrays and strings remain valid until the next successful
 * font change or component destruction.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_font_specification(
    const boxrig_x11_terminal_component *component,
    boxrig_font_specification *specification_out);

/**
 * Transactionally changes output DPI and rasterization policy.
 *
 * @param component Component whose output metrics change together.
 * @param policy Candidate policy copied on success.
 * @return BOXRIG_UI_UNAVAILABLE for a model metric conflict, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_font_rendering_policy(
    boxrig_x11_terminal_component *component,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the committed output font policy.
 *
 * @param component Component to inspect.
 * @param policy_out Receives the current policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_font_rendering_policy(
    const boxrig_x11_terminal_component *component,
    boxrig_font_rendering_policy *policy_out);

/**
 * Reads the renderer's committed effective font metrics.
 *
 * @param component Component to inspect.
 * @param metrics_out Receives the complete resolved metric state.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status
boxrig_x11_terminal_component_get_effective_metrics(
    const boxrig_x11_terminal_component *component,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Registers one validated application font file in this component.
 *
 * @param component Component owning the private font catalog.
 * @param path Length-delimited UTF-8 file path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based font face index.
 * @param resource_id_out Receives a nonzero exact resource identifier.
 * @return A portable font catalog status.
 *
 * @remarks Registration does not change the active font or layout.
 */
boxrig_font_catalog_status
boxrig_x11_terminal_component_register_font_file(
    boxrig_x11_terminal_component *component,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Registers copied immutable application font bytes in this component.
 *
 * @param component Component owning the private font catalog.
 * @param data Font bytes copied before this function returns.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based font face index.
 * @param resource_id_out Receives a nonzero exact resource identifier.
 * @return A portable font catalog status.
 *
 * @remarks Registration does not change the active font or layout.
 */
boxrig_font_catalog_status
boxrig_x11_terminal_component_register_font_memory(
    boxrig_x11_terminal_component *component,
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
boxrig_x11_terminal_component_primary_font_resource(
    const boxrig_x11_terminal_component *component);

/**
 * Returns the revision of this component's private font catalog.
 *
 * @param component Component to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_x11_terminal_component_font_catalog_revision(
    const boxrig_x11_terminal_component *component);

/**
 * Changes explicit extra line height without changing raw font size.
 *
 * @param component Component to configure transactionally.
 * @param adjustment_dip Nonnegative logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_x11_terminal_component_set_line_height_adjustment(
    boxrig_x11_terminal_component *component,
    float adjustment_dip);

/**
 * Reads the explicit raw line-height addition.
 *
 * @param component Component to inspect.
 * @param adjustment_dip_out Receives the logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status
boxrig_x11_terminal_component_get_line_height_adjustment(
    const boxrig_x11_terminal_component *component,
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
boxrig_ui_status boxrig_x11_terminal_component_visual_zoom(
    boxrig_x11_terminal_component *component,
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
boxrig_ui_status boxrig_x11_terminal_component_visual_zoom_reset(
    boxrig_x11_terminal_component *component,
    bool *changed_out);

/**
 * Selects automatic, forced LTR, or forced RTL row layout.
 *
 * @param component Component to configure.
 * @param policy New row direction policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_text_direction(
    boxrig_x11_terminal_component *component,
    boxrig_text_direction_policy policy);

/**
 * Returns the current row direction policy.
 *
 * @param component Component to inspect.
 * @return Current policy, or BOXRIG_TEXT_DIRECTION_AUTO for NULL.
 */
boxrig_text_direction_policy
boxrig_x11_terminal_component_text_direction(
    const boxrig_x11_terminal_component *component);

/**
 * Installs an application-loop frame request notification.
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
boxrig_x11_terminal_component_set_frame_request_callback(
    boxrig_x11_terminal_component *component,
    boxrig_ui_frame_request_callback callback,
    void *context);

/**
 * Requests focus for the owned child without raising its toplevel.
 *
 * @param component Visible component to focus.
 * @return BOXRIG_UI_UNAVAILABLE for borrowed-target hosting, where the caller
 * owns focus policy, otherwise a portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_request_focus(
    boxrig_x11_terminal_component *component);

/**
 * Reports the last native focus state dispatched by the application.
 *
 * @param component Component to inspect.
 * @return True after FocusIn and before the corresponding FocusOut.
 */
bool boxrig_x11_terminal_component_focused(
    const boxrig_x11_terminal_component *component);

/**
 * Inhibits or resumes this component's terminal-directed user input.
 * @param component Live component on its owning event thread.
 * @param enabled True to resume, false to discard pending user input.
 * @return OK; BUSY during dispatch (unchanged) or while a started paste awaits
 * output capacity (accepted); INVALID_ARGUMENT, UNAVAILABLE or LIMIT_EXCEEDED.
 * @remarks Inspect get_input_state() to distinguish an accepted STOPPING request.
 * Regular frame dispatch completes it when the closing paste sequence is queued
 * in core. Enabling during STOPPING is BUSY. Copy, selection, scrolling, drawing
 * and local routing remain available; transport draining is owned by the host.
 * Other components sharing the terminal and already queued output are unaffected.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_input_enabled(
    boxrig_x11_terminal_component *component, bool enabled);

/**
 * Reads the input phase and generation without advancing pending work.
 * @param component Live component on its owning event thread.
 * @param state_out Required state output.
 * @return OK or INVALID_ARGUMENT.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_input_state(
    const boxrig_x11_terminal_component *component, boxrig_ui_input_state *state_out);

/**
 * Exchanges input settings when no native key or IME transaction is pending.
 * @param component Frontend to configure on its owning event thread.
 * @param config Complete replacement, copied using the core validation rules.
 * @return OK, BUSY while a key is held or input/IME/clipboard/paste is pending,
 *         INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks BUSY and other failures preserve all old settings and input state.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_input_config(
    boxrig_x11_terminal_component *component, const boxrig_input_config *config);

/**
 * Installs application shortcut routing ahead of terminal encoding.
 *
 * @param component Component to configure.
 * @param router Router callback, or NULL.
 * @param context Callback context, which must be NULL without router.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_key_router(
    boxrig_x11_terminal_component *component,
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
boxrig_ui_status boxrig_x11_terminal_component_set_mouse_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Reports currently supported portable view actions.
 *
 * @param component Component to inspect.
 * @return A complete capability snapshot, or no capabilities for NULL.
 */
boxrig_ui_view_action_capabilities
boxrig_x11_terminal_component_view_action_capabilities(
    const boxrig_x11_terminal_component *component);

/**
 * Starts one shortcut-independent portable view action.
 *
 * @param component Component that owns the view and native transaction.
 * @param action Complete portable action.
 * @return A portable UI status.
 *
 * @remarks Copy ownership begins after the server-timestamp PropertyNotify is
 * forwarded. The immutable offer remains owned until SelectionClear, a later
 * offer for the same target, quiesce, or destruction.
 */
boxrig_ui_status boxrig_x11_terminal_component_apply_view_action(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_view_action *action);

/**
 * Queues one bounded user-originated UTF-8 paste transaction.
 *
 * @param component Component receiving clipboard bytes.
 * @param bytes Untrusted length-delimited UTF-8 bytes.
 * @param length Number of bytes in bytes.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_queue_paste(
    boxrig_x11_terminal_component *component,
    const uint8_t *bytes,
    size_t length);

/**
 * Maps a component-local point through retained shaped row layout.
 *
 * @param component Component to inspect.
 * @param x Horizontal 26.6 output-pixel position.
 * @param y Vertical 26.6 output-pixel position.
 * @param result_out Receives grid and logical-cell coordinates.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_hit_test(
    boxrig_x11_terminal_component *component,
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
boxrig_ui_status boxrig_x11_terminal_component_get_scrollbar_state(
    boxrig_x11_terminal_component *component, boxrig_scrollbar_state *state_out);

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
boxrig_ui_status boxrig_x11_terminal_component_set_scrollbar_visibility(
    boxrig_x11_terminal_component *component, boxrig_ui_scrollbar_visibility policy);

/**
 * Reads requested policy and committed standard-bar geometry.
 * @param component Component to inspect.
 * @param state_out Receives visibility, native-pixel width and pending state.
 * @return A portable status. This read does not dispatch or alter layout.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_scrollbar_visibility(
    const boxrig_x11_terminal_component *component, boxrig_ui_scrollbar_visibility_state *state_out);

/**
 * Reads the terminal viewport as a component-local scrollbar model.
 *
 * @param component Component to inspect.
 * @param model_out Receives the portable scrollbar model.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_scrollbar_model(
    boxrig_x11_terminal_component *component,
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
boxrig_ui_status boxrig_x11_terminal_component_apply_scrollbar_action(
    boxrig_x11_terminal_component *component,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out);

/**
 * Transactionally changes component-surface transparency.
 *
 * @param component Component to configure.
 * @param config Requested transparency and explicit scope.
 * @return BOXRIG_UI_UNAVAILABLE without changing state for an unmet required
 * request, otherwise a portable UI status.
 *
 * @remarks Native-toplevel scope is never advertised or modified here.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_transparency(
    boxrig_x11_terminal_component *component,
    const boxrig_ui_transparency_config *config);

/**
 * Reads requested, capable, and effective component transparency.
 *
 * @param component Component to inspect.
 * @param state_out Receives the resolved portable state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_transparency(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_transparency_state *state_out);

/**
 * Returns portable capabilities of this component instance.
 *
 * @param component Component to inspect.
 * @return Bitwise boxrig_ui_component_capability values, or zero for NULL.
 */
uint32_t boxrig_x11_terminal_component_capabilities(
    const boxrig_x11_terminal_component *component);

/**
 * Reads recoverable component state without logging or terminating.
 *
 * @param component Component to inspect.
 * @param diagnostic_out Receives current state and revisions.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_terminal_component_get_diagnostic(
    const boxrig_x11_terminal_component *component,
    boxrig_ui_component_diagnostic *diagnostic_out);

/**
 * Changes whether component frames may be presented.
 *
 * @param component Component to configure.
 * @param visible Whether the view may produce visible presentation.
 * @return A portable UI status.
 *
 * @remarks Owned-child hosting maps or unmaps the child. Borrowed-target
 * hosting changes only portable visibility and never maps or unmaps either
 * caller-owned target.
 */
boxrig_ui_status boxrig_x11_terminal_component_set_visible(
    boxrig_x11_terminal_component *component,
    bool visible);

/**
 * Reads the application-supplied component visibility state.
 *
 * @param component Component to inspect.
 * @return True only while configured visible.
 */
bool boxrig_x11_terminal_component_visible(
    const boxrig_x11_terminal_component *component);

/**
 * Dispatches one X event without consuming parent or sibling events.
 *
 * @param component Component receiving the event.
 * @param event One item removed by the caller-owned event loop.
 * @return Unrelated, handled, blocked, or failure.
 *
 * @remarks Offer every event removed by the caller-owned loop so Xlib can
 * filter XIM protocol events whose native target differs from the input
 * Window. For borrowed-target hosting the caller owns event selection and
 * routes keyboard, focus, XIM, and selection events to the focused component,
 * and pointer events to the component whose allocation contains the point.
 * Ordinary parent and sibling events remain unrelated. A blocked result
 * requires retrying the same event after a frame or transport write makes
 * terminal output capacity available.
 */
boxrig_ui_component_event_result
boxrig_x11_terminal_component_dispatch_event(
    boxrig_x11_terminal_component *component,
    const XEvent *event);

/**
 * Performs one bounded render, IME-placement, and scrollbar frame.
 *
 * @param component Component to dispatch after model synchronization.
 * @param result_out Receives remaining work and animation scheduling state.
 * @return A portable UI status.
 *
 * @remarks This function never waits. The application first dispatches its
 * session and calls boxrig_ui_model_synchronize(), then invokes this function.
 * A completed zoom checkpoint can promote one latest target, so the host must
 * inspect the component size-command state after every successful frame.
 */
boxrig_ui_status boxrig_x11_terminal_component_frame(
    boxrig_x11_terminal_component *component,
    boxrig_ui_component_frame_result *result_out);

#ifdef __cplusplus
}
#endif

#endif
