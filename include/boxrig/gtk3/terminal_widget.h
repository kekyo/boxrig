/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_GTK3_TERMINAL_WIDGET_H
#define BOXRIG_GTK3_TERMINAL_WIDGET_H

#include "boxrig/ui/component.h"
#include "boxrig/ui/input.h"
#include "boxrig/ui/scrollbar.h"
#include "boxrig/gtk/rendering.h"
#include "boxrig/gtk/input.h"

#include <gtk/gtk.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

/** Default number of normalized native input events retained by a widget. */
#define BOXRIG_GTK3_NATIVE_INPUT_EVENTS_DEFAULT ((size_t)256)

/** Hard maximum number of normalized native input events retained by a widget. */
#define BOXRIG_GTK3_NATIVE_INPUT_EVENTS_MAXIMUM ((size_t)4096)

/** Default maximum work items processed by one application-driven frame. */
#define BOXRIG_GTK3_FRAME_ITEMS_DEFAULT ((size_t)256)

/** Hard maximum work items processed by one application-driven frame. */
#define BOXRIG_GTK3_FRAME_ITEMS_MAXIMUM ((size_t)4096)

#define BOXRIG_GTK3_TYPE_TERMINAL_WIDGET \
    (boxrig_gtk3_terminal_widget_get_type())

/** Final GTK 3 widget presenting one independently retained terminal view. */
G_DECLARE_FINAL_TYPE(
    BoxrigGtk3TerminalWidget,
    boxrig_gtk3_terminal_widget,
    BOXRIG_GTK3,
    TERMINAL_WIDGET,
    GtkContainer)

/** Bounded creation configuration for one GTK 3 terminal widget. */
typedef struct boxrig_gtk3_terminal_widget_config {
    /** Portable retained-view limits. */
    boxrig_ui_view_config view;
    /** Canonical layout authority or presentation-only attachment. */
    boxrig_ui_component_attachment attachment;
    /** Maximum accepted native clipboard payload in bytes. */
    size_t paste_capacity;
    /** Preferred paste bytes; a UTF-8 scalar is never split. */
    size_t paste_chunk_size;
    /** Maximum normalized native input events retained by the widget. */
    size_t maximum_native_input_events;
    /** Maximum queued input and callback items processed by one frame. */
    size_t maximum_frame_items;
    /** Maximum simultaneous native clipboard transfers. */
    size_t maximum_clipboard_transfers;
    /** Maximum aggregate outgoing clipboard bytes. */
    size_t maximum_clipboard_bytes;
    /** Initial component-surface transparency request. */
    boxrig_ui_transparency_config transparency;
    /** Terminal rendering policy; Cairo is the default. */
    boxrig_gtk_render_policy render_policy;
    /** Input-method route, selected independently of the renderer. */
    boxrig_gtk_ime_policy ime_policy;
    /** Candidate display preference; AUTO uses a supported direct IBus renderer. */
    boxrig_ui_candidate_policy candidate_policy;
    /** Optional application-loop frame request notification. */
    boxrig_ui_frame_request_callback frame_request_callback;
    /** Context passed to frame_request_callback. */
    void *frame_request_context;
} boxrig_gtk3_terminal_widget_config;

/**
 * Returns conservative GTK 3 widget defaults.
 *
 * @return Layout-authority configuration with bounded input, frame, and
 * clipboard resources.
 */
boxrig_gtk3_terminal_widget_config
boxrig_gtk3_terminal_widget_config_default(void);

/**
 * Creates one GTK 3 terminal widget transactionally.
 *
 * @param model Caller-owned model that outlives widget disposal.
 * @param config Bounded configuration copied during creation.
 * @param widget_out Receives a floating GTK widget reference on success.
 * @return BOXRIG_UI_UNAVAILABLE for an authority or metric conflict, otherwise
 * a portable UI status.
 *
 * @remarks The widget borrows model and terminal. It owns its view, private
 * renderer, immutable presentation surface, adjustments, native event
 * connections, input method, and clipboard transactions.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_create(
    boxrig_ui_model *model,
    const boxrig_gtk3_terminal_widget_config *config,
    BoxrigGtk3TerminalWidget **widget_out);

/**
 * Creates a widget and retains diagnostics even when creation fails.
 *
 * @param model Borrowed model that outlives widget disposal.
 * @param config Configuration copied during creation.
 * @param widget_out Receives a floating widget reference, or NULL on failure.
 * @param state_out Receives the requested policy and creation result.
 * @return A portable UI status; an unavailable required build returns failure.
 * @remarks GTK creates its context only after the widget is realized. A
 * successful creation initially reports backend NONE; call frame after native
 * allocation and inspect get_render_state for initialization and draw results.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_create_with_render_state(
    boxrig_ui_model *model,
    const boxrig_gtk3_terminal_widget_config *config,
    BoxrigGtk3TerminalWidget **widget_out,
    boxrig_gtk_render_state *state_out);

/**
 * Reads requested policy, completed backend, and the last selection diagnostic.
 *
 * @param widget Widget owned by the current GTK thread.
 * @param state_out Receives a bounded snapshot, including fallback failures.
 * @return A portable UI status.
 * @remarks A required GL failure is returned by frame when no previous backend
 * remains usable. A failed switch can retain its previous backend. Initialization
 * before the first completed image is represented by backend NONE and pending.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_render_state(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_gtk_render_state *state_out);

/**
 * Requests a rendering policy without replacing the terminal or view.
 *
 * @param widget Widget on its owning GTK thread.
 * @param policy Cairo, OpenGL preferred, or OpenGL required.
 * @return OK when accepted, INVALID_ARGUMENT for invalid input, BUSY during a
 * frame or GTK rendering, or UNAVAILABLE when OpenGL was not built.
 *
 * @remarks Continue frame processing and GTK event dispatch while the rendering
 * state's pending flag is true. The active backend changes only after a full
 * image succeeds. A failed request retains a usable previous backend and
 * reports its diagnostic in get_render_state. Reselecting a failed GL policy
 * explicitly retries initialization; ordinary frames do not retry it.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_render_policy(
    BoxrigGtk3TerminalWidget *widget, boxrig_gtk_render_policy policy);

/**
 * Stops new native callbacks and frame work without dropping the GTK object.
 *
 * @param widget Widget to quiesce.
 * @return A portable UI status. Repeated calls are harmless.
 *
 * @remarks Shutdown is irreversible and synchronously releases layout
 * authority so another component can replace this widget.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_quiesce(
    BoxrigGtk3TerminalWidget *widget);

/**
 * Returns the portable view owned by a widget.
 *
 * @param widget Widget to inspect.
 * @return Borrowed view, or NULL.
 */
boxrig_ui_view *boxrig_gtk3_terminal_widget_view(
    BoxrigGtk3TerminalWidget *widget);

/**
 * Returns the caller-owned model borrowed by a widget.
 *
 * @param widget Widget to inspect.
 * @return Borrowed model, or NULL after disposal.
 */
boxrig_ui_model *boxrig_gtk3_terminal_widget_model(
    BoxrigGtk3TerminalWidget *widget);

/**
 * Supplies the current native window state used by later allocations.
 *
 * @param widget Widget to configure.
 * @param window_state Current toplevel or embedding state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_window_state(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_window_state window_state);

/**
 * Applies or clears optional cell-grid geometry hints on an explicit host.
 *
 * @param widget Widget providing the current effective cell metrics.
 * @param window Application-owned normal toplevel window.
 * @param enabled Whether minimum, base-size, and resize-increment hints are
 * enabled.
 * @return BOXRIG_UI_LIMIT_EXCEEDED when a logical cell does not fit GTK's
 * geometry fields, otherwise a portable UI status.
 *
 * @remarks The helper never searches widget ancestors, resizes the window, or
 * makes hints a correctness requirement. Hints are cleared while the widget is
 * maximized, fullscreen, tiled, or embedded. The application remains
 * responsible for passing every actual content allocation to the widget.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_geometry_hints(
    BoxrigGtk3TerminalWidget *widget,
    GtkWindow *window,
    bool enabled);

/**
 * Reads the last effective allocation and grid.
 *
 * @param widget Widget to inspect.
 * @param layout_out Receives the effective layout.
 * @return BOXRIG_UI_INVALID_ARGUMENT before initial allocation, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_layout(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_layout_result *layout_out);

/**
 * Changes the preferred normal-state terminal grid.
 *
 * @param widget Current layout-authority widget.
 * @param rows Preferred nonzero rows.
 * @param columns Preferred nonzero columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_preferred_grid(
    BoxrigGtk3TerminalWidget *widget,
    size_t rows,
    size_t columns);

/**
 * Reads the preferred normal-state terminal grid.
 *
 * @param widget Widget to inspect.
 * @param rows_out Receives preferred rows.
 * @param columns_out Receives preferred columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_preferred_grid(
    const BoxrigGtk3TerminalWidget *widget,
    size_t *rows_out,
    size_t *columns_out);

/**
 * Reads the current application size-command lifecycle.
 *
 * @param widget Widget to inspect.
 * @param state_out Receives IDLE, READY, or DISPATCHED.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_size_command_state(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_size_command_state *state_out);

/**
 * Takes the one currently executable component layout action.
 *
 * @param widget Current layout-authority widget.
 * @param action_out Receives a tagged content-size or grid-resize action.
 * @return True only when one READY action advances to DISPATCHED.
 *
 * @remarks Content-size has priority over grid resize. Grid actions are
 * exposed only after their native allocation and draw boundary is ready.
 * Complete the returned action with the kind-specific completion operation.
 */
bool boxrig_gtk3_terminal_widget_take_layout_action(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_layout_action *action_out);

/**
 * Completes one application size command using the actual host allocation.
 *
 * @param widget Widget containing the dispatched command.
 * @param revision Command revision echoed by the host.
 * @param outcome Accepted, constrained, rejected, or superseded result.
 * @param allocation_width Actual content width in 26.6 output pixels.
 * @param allocation_height Actual content height in 26.6 output pixels.
 * @param applied_out Receives false for an obsolete revision.
 * @param layout_out Receives the resulting effective layout.
 * @return A portable UI status.
 * @remarks For asynchronous GtkWindow resizing, complete after the host has
 * allocated its children, not merely after a configure-event notification.
 * An ordinary handler for the owned toplevel's size-allocate signal runs after
 * its class handler and can observe the actual terminal allocation, including
 * an unchanged allocation after rejection. No additional draw wait is needed
 * here; the widget owns presentation readiness for the resulting grid resize.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_complete_size_command(
    BoxrigGtk3TerminalWidget *widget,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out);

/**
 * Records the application-owned result of one dispatched grid resize.
 *
 * @param widget Widget that returned the request.
 * @param revision Revision returned by take_layout_action.
 * @param succeeded Whether both transport and terminal resize succeeded.
 * @return BOXRIG_UI_OK when the result is recorded, BOXRIG_UI_STALE for an
 * unknown or completed revision, or BOXRIG_UI_INVALID_ARGUMENT for invalid
 * arguments, a stopped widget, or a success whose terminal grid does not match.
 *
 * @remarks Apply the request to the application-owned session first, complete
 * that same revision, synchronize the model, and call frame normally. Only one
 * request may be dispatched at a time; a later allocation is retained separately.
 * Completing the dispatched revision never completes that later request.
 * Successful completion requests dispatch even when view dirty is already
 * pending. Callbacks only schedule work and must not reenter frame or session
 * processing from the native draw or completion call.
 * Failure quiesces the widget while retaining its last image. The application
 * must handle session-fatal shutdown; this operation neither owns the session
 * nor rolls back an external transport effect. A hidden widget still accepts
 * completion, but a quiesced widget does not. Call on the owning GTK thread.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_complete_grid_resize(
    BoxrigGtk3TerminalWidget *widget,
    uint64_t revision,
    bool succeeded);

/**
 * Transactionally changes the raw portable font specification.
 *
 * @param widget Widget whose renderer and layout metrics change together.
 * @param specification Candidate specification copied on success.
 * @return BOXRIG_UI_UNAVAILABLE for a model metric conflict, otherwise a
 * portable UI status.
 *
 * @remarks Candidate resolution and canonical metric validation finish before
 * the renderer and stored specification are committed.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_font_specification(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_specification *specification);

/**
 * Reads the current raw portable font specification.
 *
 * @param widget Widget to inspect.
 * @param specification_out Receives a descriptor borrowing widget storage.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_font_specification(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_specification *specification_out);

/**
 * Transactionally changes output DPI and rasterization policy.
 *
 * @param widget Widget whose renderer policy changes.
 * @param policy Candidate policy; output_scale must match the current GDK
 * backing scale.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_font_rendering_policy(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the committed output font policy.
 *
 * @param widget Widget to inspect.
 * @param policy_out Receives current policy including GDK backing scale.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_font_rendering_policy(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_rendering_policy *policy_out);

/**
 * Reads committed effective font metrics.
 *
 * @param widget Widget to inspect.
 * @param metrics_out Receives the complete metric state.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status
boxrig_gtk3_terminal_widget_get_effective_metrics(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Registers one validated application font file in the private renderer.
 *
 * @param widget Widget owning the private font catalog.
 * @param path Length-delimited UTF-8 file path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based face index.
 * @param resource_id_out Receives a nonzero resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_gtk3_terminal_widget_register_font_file(
    BoxrigGtk3TerminalWidget *widget,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Registers copied immutable application font bytes in the private renderer.
 *
 * @param widget Widget owning the private font catalog.
 * @param data Font bytes copied before return.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based face index.
 * @param resource_id_out Receives a nonzero resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_gtk3_terminal_widget_register_font_memory(
    BoxrigGtk3TerminalWidget *widget,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Returns the active exact application font resource.
 *
 * @param widget Widget to inspect.
 * @return Resource identifier, or BOXRIG_FONT_RESOURCE_NONE.
 */
boxrig_font_resource_id
boxrig_gtk3_terminal_widget_primary_font_resource(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Returns the revision of the widget-private font catalog.
 *
 * @param widget Widget to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_gtk3_terminal_widget_font_catalog_revision(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Changes explicit extra line height without changing raw font size.
 *
 * @param widget Widget to configure transactionally.
 * @param adjustment_dip Nonnegative logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_line_height_adjustment(
    BoxrigGtk3TerminalWidget *widget,
    float adjustment_dip);

/**
 * Reads the explicit raw line-height addition.
 *
 * @param widget Widget to inspect.
 * @param adjustment_dip_out Receives the logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_line_height_adjustment(
    const BoxrigGtk3TerminalWidget *widget,
    float *adjustment_dip_out);

/**
 * Applies one lazily resolved deterministic visual zoom step.
 *
 * @param widget Widget to configure transactionally.
 * @param direction Increase or decrease direction.
 * @param policy Candidate range and increment.
 * @param changed_out Receives true only when the desired target changes.
 * @return A portable UI status.
 *
 * @remarks Visual zoom does not mutate the queried raw font specification. One
 * presentation generation remains in flight; repeated calls retain only the
 * latest pending target.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_visual_zoom(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out);

/**
 * Resets visual zoom to the raw font specification.
 *
 * @param widget Widget to configure transactionally.
 * @param changed_out Receives true only when the desired target changes.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_visual_zoom_reset(
    BoxrigGtk3TerminalWidget *widget,
    bool *changed_out);

/**
 * Selects automatic, forced LTR, or forced RTL row layout.
 *
 * @param widget Widget to configure.
 * @param policy New row direction policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_text_direction(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_text_direction_policy policy);

/**
 * Returns the current row direction policy.
 *
 * @param widget Widget to inspect.
 * @return Current policy, or BOXRIG_TEXT_DIRECTION_AUTO for NULL.
 */
boxrig_text_direction_policy boxrig_gtk3_terminal_widget_text_direction(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Installs an application-loop frame request notification.
 *
 * @param widget Widget to configure.
 * @param callback Dirty or native-progress notification, or NULL.
 * @param context Callback context, which must be NULL without callback.
 * @return A portable UI status.
 *
 * @remarks The callback only schedules a later application-loop dispatch.
 * Do not reenter frame, model synchronization, or transport operations from
 * this callback or a GTK draw handler. Native resize readiness can notify even
 * while dirty is already dirty; notifications are not frame completions.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_frame_request_callback(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_frame_request_callback callback,
    void *context);

/**
 * Installs an application key router at the portable transition boundary.
 *
 * @param widget Widget to configure.
 * @param router Router callback, or NULL for terminal routing only.
 * @param context Context passed to router; NULL when router is NULL.
 * @return BOXRIG_UI_BUSY during an active physical key series, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_key_router(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_input_key_router router,
    void *context);

/**
 * Installs an application pointer router at the portable transition boundary.
 *
 * @param widget Widget to configure.
 * @param router Router callback, or NULL for terminal routing only.
 * @param context Context passed to router; NULL when router is NULL.
 * @return BOXRIG_UI_BUSY during an active button series, otherwise a portable
 * UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_mouse_router(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Returns portable view actions currently supported by the widget.
 *
 * @param widget Widget to inspect.
 * @return Complete view-action capability snapshot.
 */
boxrig_ui_view_action_capabilities
boxrig_gtk3_terminal_widget_view_action_capabilities(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Starts one explicit portable view action.
 *
 * @param widget Widget whose view and clipboard are used.
 * @param action Complete portable action.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_apply_view_action(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_ui_view_action *action);

/**
 * Copies and queues one bounded user-originated UTF-8 paste transaction.
 *
 * @param widget Widget whose terminal receives the transaction.
 * @param bytes Length-delimited candidate bytes.
 * @param length Number of bytes in bytes.
 * @return A portable UI status. Invalid UTF-8 and over-limit values are never
 * partially queued.
 *
 * @remarks The transaction advances by at most the configured frame-item
 * limit before return. Backpressured or remaining phases resume in frame().
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_queue_paste(
    BoxrigGtk3TerminalWidget *widget,
    const uint8_t *bytes,
    size_t length);

/**
 * Maps one output-pixel position through terminal and active IME overlays.
 *
 * @param widget Widget to inspect.
 * @param x Horizontal fixed-point output-pixel coordinate.
 * @param y Vertical fixed-point output-pixel coordinate.
 * @param result_out Receives the portable hit result.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_hit_test(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out);

/**
 * Returns the widget-owned transient input-method composition.
 *
 * @param widget Widget to inspect.
 * @return Borrowed composition, or NULL after disposal.
 */
boxrig_composition *boxrig_gtk3_terminal_widget_composition(
    BoxrigGtk3TerminalWidget *widget);

/**
 * Changes application-supplied presentation visibility.
 *
 * @param widget Widget to configure.
 * @param visible Whether application-driven frames may be presented.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_visible(
    BoxrigGtk3TerminalWidget *widget,
    bool visible);

/**
 * Reads application-supplied presentation visibility.
 *
 * @param widget Widget to inspect.
 * @return True only while configured visible.
 */
bool boxrig_gtk3_terminal_widget_visible(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Reads the shared scrollbar state for an application-owned external bar.
 * @param widget Widget to inspect.
 * @param state_out Receives the last synchronized values and shared revision.
 * @return A portable UI status.
 * @remarks Synchronize the UI model after direct terminal mutations. Read this
 * state in the scheduled frame and update the external bar only when its
 * revision changes. The getter neither moves the viewport nor emits callbacks.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_scrollbar_state(
    BoxrigGtk3TerminalWidget *widget, boxrig_scrollbar_state *state_out);

/**
 * Creates the standard non-overlay scrolled container and parents the widget.
 * @param widget Unparented widget, borrowed by this call.
 * @return A new floating GtkScrolledWindow, or NULL if already parented/invalid.
 * @remarks The caller sinks/parents and destroys the returned container. It
 * owns its child reference; the widget keeps only a weak container reference.
 * Keep the widget's model alive through widget disposal. To use a custom
 * container, omit this call. Only this factory's container is controlled by
 * the visibility API. Do not replace its policies or overlay setting directly.
 */
GtkScrolledWindow *boxrig_gtk3_terminal_widget_create_scroll_container(BoxrigGtk3TerminalWidget *widget);

/**
 * Requests ALWAYS (default), AUTO while history is navigable, or NEVER.
 * @param widget Widget to change, on its owning GTK thread.
 * @param policy Requested standard-bar visibility.
 * @return A portable status; BUSY leaves the policy unchanged during layout.
 * @remarks Continue GTK dispatch, frame and layout actions. A required grid
 * resize completes before GTK applies the new bar policy. Caller-created
 * containers are never changed. Without a standard container only the policy
 * preference is stored. AUTO resolves through the common history model.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_scrollbar_visibility(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_scrollbar_visibility policy);

/**
 * Reads policy and the last committed standard-bar allocation.
 * @param widget Widget to inspect on its owning GTK thread.
 * @param state_out Receives state; reserved_width is in GTK logical pixels.
 * @return A portable status. An external or removed container reserves zero.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_scrollbar_visibility(
    const BoxrigGtk3TerminalWidget *widget, boxrig_ui_scrollbar_visibility_state *state_out);

/**
 * Reads the terminal viewport as a portable scrollbar model.
 *
 * @param widget Widget to inspect.
 * @param model_out Receives the model.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_scrollbar_model(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_scrollbar_model *model_out);

/**
 * Applies one portable scrollbar operation.
 *
 * @param widget Widget whose viewport is controlled.
 * @param action Portable scrollbar action.
 * @param position Absolute position for BOXRIG_SCROLLBAR_POSITION.
 * @param changed_out Receives whether the viewport changed.
 * @return A portable UI status.
 * @remarks A changed position synchronizes the shared UI model and requests
 * frames. Repeated positions, absent history and alternate screens are no-ops.
 * changed_out remains true if a move is followed by a synchronization error.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_apply_scrollbar_action(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_scrollbar_action action,
    size_t position,
    bool *changed_out);

/**
 * Changes component-surface transparency transactionally.
 *
 * @param widget Widget to configure.
 * @param config Requested transparency and explicit scope.
 * @return BOXRIG_UI_UNAVAILABLE for an unmet required request, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_set_transparency(
    BoxrigGtk3TerminalWidget *widget,
    const boxrig_ui_transparency_config *config);

/**
 * Reads requested, capable, and effective component transparency.
 *
 * @param widget Widget to inspect.
 * @param state_out Receives resolved state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_transparency(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_transparency_state *state_out);

/**
 * Returns portable capabilities of this widget instance.
 *
 * @param widget Widget to inspect.
 * @return Bitwise boxrig_ui_component_capability values, or zero for NULL.
 */
uint32_t boxrig_gtk3_terminal_widget_capabilities(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Reads recoverable component state without logging or terminating.
 *
 * @param widget Widget to inspect.
 * @param diagnostic_out Receives current state and revisions.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_diagnostic(
    const BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_diagnostic *diagnostic_out);

/**
 * Performs one bounded retained render after model synchronization.
 *
 * @param widget Widget to dispatch from its creation main context.
 * @param result_out Receives remaining work and animation scheduling state.
 * @return A portable UI status.
 *
 * @remarks This function never waits. GtkWidget::draw only paints an immutable
 * retained surface and never consumes model dirty. Allocation may publish a
 * background-filled preview of the last complete image without rendering a new
 * terminal grid. While a size command, native grid boundary, or dispatched grid
 * resize is unresolved, frame preserves that safe image and advances only input
 * whose geometry is usable. A first frame may render the current core grid so
 * initial allocation does not wait circularly for its own resize completion.
 *
 * frame_pending describes remaining presentation work, including native and
 * application resize waits. Only additional_dispatch requests immediate work;
 * otherwise wait for native events, the scheduling callback, or a deadline.
 * A failed allocation or preview preparation retains the old image and can be
 * retried by a later frame; continued failure returns a non-OK status without
 * fabricating a native boundary or installing a retry loop. Callbacks schedule
 * dispatch only and must not reenter this operation or perform session work.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_frame(
    BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_frame_result *result_out);

/**
 * Reports whether one immutable presentation has been committed.
 *
 * @param widget Widget to inspect.
 * @return True after the first successful application-driven frame.
 */
bool boxrig_gtk3_terminal_widget_has_presentation(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Returns the last immutable presentation generation.
 *
 * @param widget Widget to inspect.
 * @return Nonzero committed generation, or zero before the first frame.
 *
 * @remarks This tracks immutable image replacement, including allocation-time
 * resize previews, not only full terminal rendering. It does not acknowledge
 * a GTK draw boundary or physical display completion; use the grid-resize
 * take/complete protocol to synchronize application-owned transport instead.
 */
uint64_t boxrig_gtk3_terminal_widget_presentation_generation(
    const BoxrigGtk3TerminalWidget *widget);

/**
 * Reads input selection and connection diagnostics without dispatching events.
 *
 * @param widget Live terminal widget, accessed on its GTK thread.
 * @param state_out Receives a snapshot with no borrowed pointers.
 * @return BOXRIG_UI_OK or BOXRIG_UI_INVALID_ARGUMENT.
 * @remarks Before attachment, the route is PENDING and no display is queried.
 * Registry version is what the server advertises, not GTK's private binding.
 * candidate is valid only for a focused direct IBus context after notification.
 */
boxrig_ui_status boxrig_gtk3_terminal_widget_get_ime_state(
    const BoxrigGtk3TerminalWidget *widget, boxrig_gtk_ime_state *state_out);

G_END_DECLS

#endif
