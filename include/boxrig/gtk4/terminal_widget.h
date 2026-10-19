/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_GTK4_TERMINAL_WIDGET_H
#define BOXRIG_GTK4_TERMINAL_WIDGET_H

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
#define BOXRIG_GTK4_NATIVE_INPUT_EVENTS_DEFAULT ((size_t)256)

/** Hard maximum number of normalized native input events retained by a widget. */
#define BOXRIG_GTK4_NATIVE_INPUT_EVENTS_MAXIMUM ((size_t)4096)

/** Default maximum work items processed by one application-driven frame. */
#define BOXRIG_GTK4_FRAME_ITEMS_DEFAULT ((size_t)256)

/** Hard maximum work items processed by one application-driven frame. */
#define BOXRIG_GTK4_FRAME_ITEMS_MAXIMUM ((size_t)4096)

#define BOXRIG_GTK4_TYPE_TERMINAL_WIDGET \
    (boxrig_gtk4_terminal_widget_get_type())

/** Final GTK 4 widget presenting one independently retained terminal view. */
G_DECLARE_FINAL_TYPE(
    BoxrigGtk4TerminalWidget,
    boxrig_gtk4_terminal_widget,
    BOXRIG_GTK4,
    TERMINAL_WIDGET,
    GtkWidget)

/** Bounded creation configuration for one GTK 4 terminal widget. */
typedef struct boxrig_gtk4_terminal_widget_config {
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
    /** Input-method policy, independent of rendering; AUTO is the default. */
    boxrig_gtk_ime_policy ime_policy;
    /** Candidate display preference; AUTO uses a supported direct IBus renderer. */
    boxrig_ui_candidate_policy candidate_policy;
    /** Publish terminal text to assistive technologies; disabled by default. */
    bool accessibility_enabled;
    /** Optional application-loop frame request notification. */
    boxrig_ui_frame_request_callback frame_request_callback;
    /** Context passed to frame_request_callback. */
    void *frame_request_context;
    /**
     * Optional initial font, copied during creation; NULL uses font defaults.
     * Resolve the same metrics as an existing model before attaching a new view.
     * Family names/features/variations need remain valid only for create().
     * Resource identifiers belong to the new, initially empty font catalog.
     */
    const boxrig_font_specification *initial_font;
} boxrig_gtk4_terminal_widget_config;

/**
 * Returns conservative GTK 4 widget defaults.
 *
 * @return Layout-authority configuration with bounded input, frame, and
 * clipboard resources.
 */
boxrig_gtk4_terminal_widget_config
boxrig_gtk4_terminal_widget_config_default(void);

/**
 * Enables or disables terminal text publication to assistive technologies.
 * @param widget Live widget owned by the current GTK thread.
 * @param enabled True to publish text, caret, selections and attributes.
 * @return OK, INVALID_ARGUMENT for an invalid owner, or BUSY during callbacks.
 * @remarks Disabled by default. Disabling immediately withdraws published text
 * and stops snapshot generation. Enabling requests a frame to publish the
 * current viewport; synchronized output waits for its completed batch.
 * Standard GTK widget roles, controls and input remain available.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_accessibility_enabled(
    BoxrigGtk4TerminalWidget *widget, bool enabled);

/**
 * Returns the requested terminal text publication setting.
 * @param widget Widget on its owning GTK thread, or NULL.
 * @return True when enabled, false for NULL or a disabled widget.
 */
bool boxrig_gtk4_terminal_widget_get_accessibility_enabled(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Creates one GTK 4 terminal widget transactionally.
 *
 * @param model Caller-owned model that outlives widget disposal.
 * @param config Bounded configuration copied during creation.
 * @param widget_out Receives a floating GTK widget reference on success.
 * @return BOXRIG_UI_UNAVAILABLE for an authority or metric conflict, otherwise
 * a portable UI status.
 *
 * @remarks The widget borrows model and terminal. It owns its view, private
 * renderer, immutable presentation node, adjustments, native controllers,
 * input method, and clipboard transactions.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_create(
    boxrig_ui_model *model,
    const boxrig_gtk4_terminal_widget_config *config,
    BoxrigGtk4TerminalWidget **widget_out);

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
boxrig_ui_status boxrig_gtk4_terminal_widget_create_with_render_state(
    boxrig_ui_model *model,
    const boxrig_gtk4_terminal_widget_config *config,
    BoxrigGtk4TerminalWidget **widget_out,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_get_render_state(
    const BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_set_render_policy(
    BoxrigGtk4TerminalWidget *widget, boxrig_gtk_render_policy policy);

/**
 * Stops new native callbacks and frame work without dropping the GTK object.
 *
 * @param widget Widget to quiesce.
 * @return A portable UI status. Repeated calls are harmless.
 *
 * @remarks Shutdown is irreversible and synchronously releases layout
 * authority so another component can replace this widget.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_quiesce(
    BoxrigGtk4TerminalWidget *widget);

/**
 * Returns the portable view owned by a widget.
 *
 * @param widget Widget to inspect.
 * @return Borrowed view, or NULL.
 */
boxrig_ui_view *boxrig_gtk4_terminal_widget_view(
    BoxrigGtk4TerminalWidget *widget);

/**
 * Returns the caller-owned model borrowed by a widget.
 *
 * @param widget Widget to inspect.
 * @return Borrowed model, or NULL after disposal.
 */
boxrig_ui_model *boxrig_gtk4_terminal_widget_model(
    BoxrigGtk4TerminalWidget *widget);

/**
 * Supplies the current native window state used by later allocations.
 *
 * @param widget Widget to configure.
 * @param window_state Current toplevel or embedding state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_window_state(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_window_state window_state);

/**
 * Reads the last effective allocation and grid.
 *
 * @param widget Widget to inspect.
 * @param layout_out Receives the effective layout.
 * @return BOXRIG_UI_INVALID_ARGUMENT before initial allocation, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_layout(
    const BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_layout_result *layout_out);

/**
 * Changes the preferred normal-state terminal grid.
 *
 * @param widget Current layout-authority widget.
 * @param rows Preferred nonzero rows.
 * @param columns Preferred nonzero columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_preferred_grid(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_get_preferred_grid(
    const BoxrigGtk4TerminalWidget *widget,
    size_t *rows_out,
    size_t *columns_out);

/**
 * Reads the current application size-command lifecycle.
 *
 * @param widget Widget to inspect.
 * @param state_out Receives IDLE, READY, or DISPATCHED.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_size_command_state(
    const BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_size_command_state *state_out);

/**
 * Takes the one currently executable component layout action.
 *
 * @param widget Current layout-authority widget.
 * @param action_out Receives a tagged content-size or grid-resize action.
 * @return True only when one READY action advances to DISPATCHED.
 *
 * @remarks Content-size has priority over grid resize. A grid resize becomes
 * executable only after GTK crosses the native frame boundary for its actual
 * allocation. Complete the returned action before taking another.
 */
bool boxrig_gtk4_terminal_widget_take_layout_action(
    BoxrigGtk4TerminalWidget *widget,
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
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_complete_size_command(
    BoxrigGtk4TerminalWidget *widget,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *layout_out);

/**
 * Records the application-owned result of one dispatched grid resize.
 *
 * @param widget Widget that returned the action.
 * @param revision Revision returned by take_layout_action.
 * @param succeeded Whether transport and terminal resize both succeeded.
 * @return BOXRIG_UI_OK on completion, BOXRIG_UI_STALE for an unknown or
 * completed revision, or BOXRIG_UI_INVALID_ARGUMENT for invalid state or a
 * successful completion whose terminal grid does not match the action.
 *
 * @remarks Failure quiesces the widget because an external transport resize
 * cannot be rolled back safely.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_complete_grid_resize(
    BoxrigGtk4TerminalWidget *widget,
    uint64_t revision,
    bool succeeded);

/**
 * Reports whether an application-driven frame may cross the GTK boundary.
 *
 * @param widget Widget to inspect and coalesce for pending layout work.
 * @return True when the widget is allocated and no grid-resize transaction
 * must cross a GTK tick and application completion first.
 *
 * @remarks A false result is a native wait, not an immediate dispatch request.
 * The widget's frame-request callback runs when the wait can advance.
 */
bool boxrig_gtk4_terminal_widget_frame_ready(
    BoxrigGtk4TerminalWidget *widget);

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
boxrig_ui_status boxrig_gtk4_terminal_widget_set_font_specification(
    BoxrigGtk4TerminalWidget *widget,
    const boxrig_font_specification *specification);

/**
 * Reads the current raw portable font specification.
 *
 * @param widget Widget to inspect.
 * @param specification_out Receives a descriptor borrowing widget storage.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_font_specification(
    const BoxrigGtk4TerminalWidget *widget,
    boxrig_font_specification *specification_out);

/**
 * Transactionally changes output DPI and rasterization policy.
 *
 * @param widget Widget whose renderer policy changes.
 * @param policy Candidate policy; output_scale must match the current GDK
 * backing scale.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_font_rendering_policy(
    BoxrigGtk4TerminalWidget *widget,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the committed output font policy.
 *
 * @param widget Widget to inspect.
 * @param policy_out Receives current policy including GDK backing scale.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_font_rendering_policy(
    const BoxrigGtk4TerminalWidget *widget,
    boxrig_font_rendering_policy *policy_out);

/**
 * Reads committed effective font metrics.
 *
 * @param widget Widget to inspect.
 * @param metrics_out Receives the complete metric state.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status
boxrig_gtk4_terminal_widget_get_effective_metrics(
    const BoxrigGtk4TerminalWidget *widget,
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
boxrig_font_catalog_status boxrig_gtk4_terminal_widget_register_font_file(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_font_catalog_status boxrig_gtk4_terminal_widget_register_font_memory(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_gtk4_terminal_widget_primary_font_resource(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Returns the revision of the widget-private font catalog.
 *
 * @param widget Widget to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_gtk4_terminal_widget_font_catalog_revision(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Changes explicit extra line height without changing raw font size.
 *
 * @param widget Widget to configure transactionally.
 * @param adjustment_dip Nonnegative logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_line_height_adjustment(
    BoxrigGtk4TerminalWidget *widget,
    float adjustment_dip);

/**
 * Reads the explicit raw line-height addition.
 *
 * @param widget Widget to inspect.
 * @param adjustment_dip_out Receives the logical-pixel addition.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_line_height_adjustment(
    const BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_visual_zoom(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_visual_zoom_reset(
    BoxrigGtk4TerminalWidget *widget,
    bool *changed_out);

/**
 * Selects automatic, forced LTR, or forced RTL row layout.
 *
 * @param widget Widget to configure.
 * @param policy New row direction policy.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_text_direction(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_text_direction_policy policy);

/**
 * Returns the current row direction policy.
 *
 * @param widget Widget to inspect.
 * @return Current policy, or BOXRIG_TEXT_DIRECTION_AUTO for NULL.
 */
boxrig_text_direction_policy boxrig_gtk4_terminal_widget_text_direction(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Installs an application-loop frame request notification.
 *
 * @param widget Widget to configure.
 * @param callback View-dirty or native-input dispatch callback, or NULL.
 * @param context Callback context, which must be NULL without callback.
 * @return A portable UI status.
 * @remarks Dirty and native input can independently request dispatch before
 * the next frame. The host may combine these notifications into one wakeup.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_frame_request_callback(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_frame_request_callback callback,
    void *context);

/**
 * Enables or inhibits terminal-directed user input for this widget.
 * @param widget Live widget on its owning event thread.
 * @param enabled True to resume, false to discard old keys, IME and incoming paste.
 * @return OK when complete; BUSY while a started paste awaits output capacity or
 * when called during input dispatch; INVALID_ARGUMENT, UNAVAILABLE or LIMIT_EXCEEDED.
 * @remarks Inspect get_input_state(): STOPPING means the stop was accepted and
 * regular frame dispatch will finish it. Enabling during STOPPING returns BUSY
 * without changing the request. A BUSY call during input dispatch leaves the
 * state unchanged. DISABLED means any paste terminator is queued in core; it
 * does not mean transport output was drained. Queued core output and terminal
 * responses remain intact. Selection, copy, scrolling, rendering and application
 * routers remain usable. Late input from an earlier generation is discarded;
 * keys held across a stop remain suppressed until released. This affects only
 * this widget, not other views sharing the terminal or direct core send calls.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_input_enabled(
    BoxrigGtk4TerminalWidget *widget, bool enabled);

/**
 * Reads this widget's input phase and generation without advancing dispatch.
 * @param widget Live widget to inspect on its owning event thread.
 * @param state_out Required input state output.
 * @return OK or INVALID_ARGUMENT.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_input_state(
    BoxrigGtk4TerminalWidget *widget, boxrig_ui_input_state *state_out);

/**
 * Exchanges input settings when no native key or IME transaction is pending.
 * @param widget Frontend to configure on its owning event thread.
 * @param config Complete replacement, copied using the core validation rules.
 * @return OK, BUSY while a key is held or input/IME/clipboard/paste is pending,
 *         INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks BUSY and other failures preserve all old settings and input state.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_input_config(
    BoxrigGtk4TerminalWidget *widget, const boxrig_input_config *config);

/**
 * Installs an application key router at the portable transition boundary.
 *
 * @param widget Widget to configure.
 * @param router Router callback, or NULL for terminal routing only.
 * @param context Context passed to router; NULL when router is NULL.
 * @return BOXRIG_UI_BUSY during an active physical key series, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_key_router(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_set_mouse_router(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_input_mouse_router router,
    void *context);

/**
 * Returns portable view actions currently supported by the widget.
 *
 * @param widget Widget to inspect.
 * @return Complete view-action capability snapshot.
 */
boxrig_ui_view_action_capabilities
boxrig_gtk4_terminal_widget_view_action_capabilities(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Starts one explicit portable view action.
 *
 * @param widget Widget whose view and clipboard are used.
 * @param action Complete portable action.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_apply_view_action(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_queue_paste(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_hit_test(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out);

/**
 * Returns the widget-owned transient input-method composition.
 *
 * @param widget Widget to inspect.
 * @return Borrowed composition, or NULL after disposal.
 */
boxrig_composition *boxrig_gtk4_terminal_widget_composition(
    BoxrigGtk4TerminalWidget *widget);

/**
 * Changes application-supplied presentation visibility.
 *
 * @param widget Widget to configure.
 * @param visible Whether application-driven frames may be presented.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_set_visible(
    BoxrigGtk4TerminalWidget *widget,
    bool visible);

/**
 * Reads application-supplied presentation visibility.
 *
 * @param widget Widget to inspect.
 * @return True only while configured visible.
 */
bool boxrig_gtk4_terminal_widget_visible(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Reads the shared scrollbar state for an application-owned external bar.
 * @param widget Widget to inspect.
 * @param state_out Receives the last synchronized values and shared revision.
 * @return A portable UI status.
 * @remarks Synchronize the UI model after direct terminal mutations. Read this
 * state in the scheduled frame and update the external bar only when its
 * revision changes. The getter neither moves the viewport nor emits callbacks.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_scrollbar_state(
    BoxrigGtk4TerminalWidget *widget, boxrig_scrollbar_state *state_out);

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
GtkScrolledWindow *boxrig_gtk4_terminal_widget_create_scroll_container(BoxrigGtk4TerminalWidget *widget);

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
boxrig_ui_status boxrig_gtk4_terminal_widget_set_scrollbar_visibility(
    BoxrigGtk4TerminalWidget *widget, boxrig_ui_scrollbar_visibility policy);

/**
 * Reads policy and the last committed standard-bar allocation.
 * @param widget Widget to inspect on its owning GTK thread.
 * @param state_out Receives state; reserved_width is in GTK logical pixels.
 * @return A portable status. An external or removed container reserves zero.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_scrollbar_visibility(
    const BoxrigGtk4TerminalWidget *widget, boxrig_ui_scrollbar_visibility_state *state_out);

/**
 * Reads the terminal viewport as a portable scrollbar model.
 *
 * @param widget Widget to inspect.
 * @param model_out Receives the model.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_scrollbar_model(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_apply_scrollbar_action(
    BoxrigGtk4TerminalWidget *widget,
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
boxrig_ui_status boxrig_gtk4_terminal_widget_set_transparency(
    BoxrigGtk4TerminalWidget *widget,
    const boxrig_ui_transparency_config *config);

/**
 * Reads requested, capable, and effective component transparency.
 *
 * @param widget Widget to inspect.
 * @param state_out Receives resolved state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_transparency(
    const BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_transparency_state *state_out);

/**
 * Returns portable capabilities of this widget instance.
 *
 * @param widget Widget to inspect.
 * @return Bitwise boxrig_ui_component_capability values, or zero for NULL.
 */
uint32_t boxrig_gtk4_terminal_widget_capabilities(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Reads recoverable component state without logging or terminating.
 *
 * @param widget Widget to inspect.
 * @param diagnostic_out Receives current state and revisions.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_get_diagnostic(
    const BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_component_diagnostic *diagnostic_out);

/**
 * Performs one bounded retained render after model synchronization.
 *
 * @param widget Widget to dispatch from its creation main context.
 * @param result_out Receives remaining work and animation scheduling state.
 * @return A portable UI status.
 *
 * @remarks This function never waits. GtkWidget snapshot only appends the
 * immutable node committed by this operation and never consumes model dirty.
 * While an application size command is unresolved, the last complete node remains
 * committed and the retained render resumes after the resulting allocation.
 */
boxrig_ui_status boxrig_gtk4_terminal_widget_frame(
    BoxrigGtk4TerminalWidget *widget,
    boxrig_ui_component_frame_result *result_out);

/**
 * Reports whether one immutable presentation has been committed.
 *
 * @param widget Widget to inspect.
 * @return True after the first successful application-driven frame.
 */
bool boxrig_gtk4_terminal_widget_has_presentation(
    const BoxrigGtk4TerminalWidget *widget);

/**
 * Returns the last immutable presentation generation.
 *
 * @param widget Widget to inspect.
 * @return Nonzero committed generation, or zero before the first frame.
 */
uint64_t boxrig_gtk4_terminal_widget_presentation_generation(
    const BoxrigGtk4TerminalWidget *widget);

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
boxrig_ui_status boxrig_gtk4_terminal_widget_get_ime_state(
    const BoxrigGtk4TerminalWidget *widget, boxrig_gtk_ime_state *state_out);

G_END_DECLS

#endif
