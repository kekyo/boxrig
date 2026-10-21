/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_VIEW_H
#define BOXRIG_UI_VIEW_H

#include "boxrig/terminal.h"
#include "boxrig/ui/color.h"
#include "boxrig/ui/blink.h"
#include "boxrig/ui/decoration.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/candidates.h"
#include "boxrig/ui/text_layout.h"
#include "boxrig/ui/scrollbar.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of fixed-point units in one output pixel. */
#define BOXRIG_UI_FIXED_ONE INT64_C(64)

/** Default maximum UTF-8 bytes owned by one copied selection. */
#define BOXRIG_UI_COPY_CAPACITY_DEFAULT ((size_t)(16u * 1024u * 1024u))

/** Hard maximum UTF-8 bytes owned by one copied selection. */
#define BOXRIG_UI_COPY_CAPACITY_MAX ((size_t)(256u * 1024u * 1024u))

/** Default maximum application text annotations retained by one view. */
#define BOXRIG_UI_ANNOTATION_CAPACITY_DEFAULT ((size_t)65536u)

/** Hard maximum application text annotations retained by one view. */
#define BOXRIG_UI_ANNOTATION_CAPACITY_MAX ((size_t)1048576u)

/** Default maximum physical rows retained by one rectangular selection. */
#define BOXRIG_UI_SELECTION_ROW_CAPACITY_DEFAULT ((size_t)4096u)

/** Hard maximum physical rows retained by one rectangular selection. */
#define BOXRIG_UI_SELECTION_ROW_CAPACITY_MAX ((size_t)65536u)

/** Default maximum logical ranges retained by one rectangular selection. */
#define BOXRIG_UI_SELECTION_RANGE_CAPACITY_DEFAULT ((size_t)65536u)

/** Hard maximum logical ranges retained by one rectangular selection. */
#define BOXRIG_UI_SELECTION_RANGE_CAPACITY_MAX ((size_t)1048576u)

/** Default upper bound for visible filtered colors retained by one view. */
#define BOXRIG_UI_COLOR_PAINT_BYTES_DEFAULT ((size_t)(64u * 1024u * 1024u))

/** Hard upper bound for visible filtered colors retained by one view. */
#define BOXRIG_UI_COLOR_PAINT_BYTES_MAX ((size_t)(256u * 1024u * 1024u))

/** Signed 26.6 fixed-point output coordinate. */
typedef int64_t boxrig_ui_fixed;

/** Portable result codes returned by terminal-view operations. */
typedef enum boxrig_ui_status {
    BOXRIG_UI_OK = 0,
    BOXRIG_UI_INVALID_ARGUMENT,
    BOXRIG_UI_OUT_OF_MEMORY,
    BOXRIG_UI_LIMIT_EXCEEDED,
    BOXRIG_UI_UNAVAILABLE,
    /** Another transaction already owns the requested action endpoint. */
    BOXRIG_UI_BUSY,
    /** A retained logical text selection no longer names the same text. */
    BOXRIG_UI_STALE
} boxrig_ui_status;

/** Shared lifetime handle for a frontend's physical-row text analysis. */
typedef struct boxrig_ui_fragment_layout_provider boxrig_ui_fragment_layout_provider;

/**
 * Analyzes one retained physical row using the frontend's display policy.
 * @param context Borrowed provider context.
 * @param terminal Read-only terminal containing the requested fragment.
 * @param screen Logical text namespace.
 * @param fragment_index Current document-order physical row index.
 * @param maximum_layout_bytes Upper bound for the returned portable layout.
 * @param layout_out Receives an owned complete-grid layout, or NULL on failure.
 * @return A portable UI status; unresolved geometry must not be guessed.
 * @remarks Called synchronously. Do not mutate the terminal, reenter selection
 * or provider registration, or destroy the provider/context during analysis.
 * No drawing, viewport movement, or native event dispatch is needed.
 */
typedef boxrig_ui_status (*boxrig_ui_fragment_layout_resolver)(void *context,
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t fragment_index, size_t maximum_layout_bytes, boxrig_text_layout **layout_out);

/**
 * Reads the current display-analysis configuration revision without side effects.
 * @param context Borrowed provider context.
 * @return Nonzero revision changed by font/direction policy changes, or zero
 * when analysis is unavailable. Terminal text changes do not change this value.
 */
typedef uint64_t (*boxrig_ui_fragment_layout_revision_reader)(void *context);

/**
 * Creates an owner handle for physical-row analysis shared by attached views.
 * @param resolver Required synchronous row analyzer.
 * @param read_revision Required read-only configuration revision callback.
 * @param context Borrowed context, valid until provider destruction.
 * @param provider_out Receives an owner handle, or NULL on failure.
 * @return OK, INVALID_ARGUMENT, or OUT_OF_MEMORY.
 */
boxrig_ui_status boxrig_ui_fragment_layout_provider_create(
    boxrig_ui_fragment_layout_resolver resolver,
    boxrig_ui_fragment_layout_revision_reader read_revision, void *context,
    boxrig_ui_fragment_layout_provider **provider_out);

/**
 * Closes analysis before releasing the owner's handle, accepting NULL.
 * @param provider Owner handle, consumed by this call.
 * @remarks Attached views retain a closed handle and never call the old
 * context again. They may be destroyed before or after the owner. The owner
 * must call this before freeing context, outside provider callbacks.
 */
void boxrig_ui_fragment_layout_provider_destroy(boxrig_ui_fragment_layout_provider *provider);

/** Distinct retained dirty planes owned by one terminal view. */
typedef enum boxrig_ui_dirty_layer {
    /** Terminal cells, cursor, colors, and row layouts in logical columns. */
    BOXRIG_UI_DIRTY_MODEL = 0,
    /** Terminal image resources and placements in logical columns. */
    BOXRIG_UI_DIRTY_IMAGE,
    /** Frontend overlays such as IME preedit in visual columns. */
    BOXRIG_UI_DIRTY_OVERLAY,
    /** Native expose, target loss, or presentation dirty in visual columns. */
    BOXRIG_UI_DIRTY_PRESENT,
    /** Animated paint changes in visual columns, retaining text layout and colors. */
    BOXRIG_UI_DIRTY_ANIMATION,
    /** Number of public dirty layers. */
    BOXRIG_UI_DIRTY_LAYER_COUNT
} boxrig_ui_dirty_layer;

/**
 * A rectangular interval in terminal row and column coordinates.
 * Columns are logical for MODEL and IMAGE, and visual for the other layers.
 */
typedef struct boxrig_ui_dirty {
    /** Zero-based first changed row. */
    size_t row;
    /** Number of consecutive changed rows. */
    size_t row_count;
    /** Zero-based first changed column. */
    size_t column;
    /** Number of consecutive changed columns. */
    size_t column_count;
} boxrig_ui_dirty;

/** Configuration for one independently consuming terminal view. */
typedef struct boxrig_ui_view_config {
    /** Initial terminal-requested text and cursor blinking policy. */
    boxrig_ui_blink_config blink;
    /** Maximum estimated bytes retained for portable row layouts. */
    size_t maximum_retained_layout_bytes;
    /**
     * Maximum projected UTF-8 bytes owned by one copied selection.
     * The default is BOXRIG_UI_COPY_CAPACITY_DEFAULT and values above
     * BOXRIG_UI_COPY_CAPACITY_MAX are rejected.
     */
    size_t maximum_copy_bytes;
    /**
     * Maximum application text annotations retained by this view.
     * The default is BOXRIG_UI_ANNOTATION_CAPACITY_DEFAULT and values above
     * BOXRIG_UI_ANNOTATION_CAPACITY_MAX are rejected.
     */
    size_t maximum_annotation_count;
    /**
     * Maximum materialized rows owned by one rectangular selection.
     * The value must be nonzero and no greater than
     * BOXRIG_UI_SELECTION_ROW_CAPACITY_MAX.
     */
    size_t maximum_selection_rows;
    /**
     * Maximum logical row ranges owned by one rectangular selection.
     * The value must be nonzero and no greater than
     * BOXRIG_UI_SELECTION_RANGE_CAPACITY_MAX.
     */
    size_t maximum_selection_ranges;
    /**
     * Maximum bytes retained for this view's visible display-color paints.
     * The default is BOXRIG_UI_COLOR_PAINT_BYTES_DEFAULT. Values above
     * BOXRIG_UI_COLOR_PAINT_BYTES_MAX are rejected. Zero prevents registration
     * of a color filter. Unregistered views do not allocate this storage.
     */
    size_t maximum_retained_color_bytes;
    /** Maximum copied background pixels in bytes; default 64 MiB, hard limit 256 MiB. */
    size_t maximum_background_image_bytes;
} boxrig_ui_view_config;

/** Opaque shared terminal model and dirty distributor. */
typedef struct boxrig_ui_model boxrig_ui_model;

/** Opaque platform-independent terminal view. */
typedef struct boxrig_ui_view boxrig_ui_view;

/** Opaque exclusive pull consumer for one model's terminal host-item queue. */
typedef struct boxrig_ui_host_item_consumer boxrig_ui_host_item_consumer;

/** Notification that a visible view needs application-loop frame dispatch. */
typedef void (*boxrig_ui_frame_request_callback)(void *context);

/** State of the native toplevel or embedding constraint. */
typedef enum boxrig_ui_window_state {
    BOXRIG_UI_WINDOW_NORMAL = 0,
    BOXRIG_UI_WINDOW_MAXIMIZED,
    BOXRIG_UI_WINDOW_FULLSCREEN,
    BOXRIG_UI_WINDOW_TILED,
    BOXRIG_UI_WINDOW_EMBEDDED
} boxrig_ui_window_state;

/** Effective fixed grid within the last actual allocation. */
typedef struct boxrig_ui_layout_result {
    /** Effective terminal rows. */
    size_t rows;
    /** Effective terminal columns. */
    size_t columns;
    /** Width occupied by the complete fixed grid. */
    boxrig_ui_fixed grid_width;
    /** Height occupied by the complete fixed grid. */
    boxrig_ui_fixed grid_height;
    /** Allocation remaining to the right of the grid. */
    boxrig_ui_fixed excess_right;
    /** Allocation remaining below the grid. */
    boxrig_ui_fixed excess_bottom;
    /** Whether native or embedding constraints determined the grid. */
    bool constrained;
} boxrig_ui_layout_result;

/** Host resolution of one application size command. */
typedef enum boxrig_ui_size_outcome {
    /** The requested allocation was accepted. */
    BOXRIG_UI_SIZE_ACCEPTED = 0,
    /** A different constrained allocation was applied. */
    BOXRIG_UI_SIZE_CONSTRAINED,
    /** The host rejected toplevel sizing and retained its allocation. */
    BOXRIG_UI_SIZE_REJECTED,
    /** A newer external allocation superseded the application command. */
    BOXRIG_UI_SIZE_SUPERSEDED
} boxrig_ui_size_outcome;

/** Capabilities fixed independently from allocation observations. */
typedef enum boxrig_ui_geometry_capability {
    /** The host can consume application-originated content-size commands. */
    BOXRIG_UI_GEOMETRY_SIZE_COMMAND = 1u << 0,
    /** Native interactive candidates can be constrained before allocation. */
    BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS = 1u << 1
} boxrig_ui_geometry_capability;

/** One observed allocation with the metrics used to interpret it. */
typedef struct boxrig_ui_allocation_input {
    /** Actual allocated view-content width in fixed-point output pixels. */
    boxrig_ui_fixed allocation_width;
    /** Actual allocated view-content height in fixed-point output pixels. */
    boxrig_ui_fixed allocation_height;
    /** Committed cell width in fixed-point output pixels. */
    boxrig_ui_fixed cell_width;
    /** Committed cell height in fixed-point output pixels. */
    boxrig_ui_fixed cell_height;
    /** Nonzero revision of the committed cell metrics. */
    uint64_t cell_metrics_revision;
    /** Current native toplevel or embedding state. */
    boxrig_ui_window_state window_state;
} boxrig_ui_allocation_input;

/** Explicit application cause of a layout change. */
typedef enum boxrig_ui_layout_change_kind {
    /** The application committed a different font geometry. */
    BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS = 0,
    /** The application committed metrics for a different native output. */
    BOXRIG_UI_LAYOUT_CHANGE_OUTPUT_METRICS,
    /** The application changed the explicit line-height adjustment. */
    BOXRIG_UI_LAYOUT_CHANGE_LINE_HEIGHT,
    /** Application-owned content chrome changed its reserved extent. */
    BOXRIG_UI_LAYOUT_CHANGE_CHROME,
    /** The application requests restoration of its normal grid anchor. */
    BOXRIG_UI_LAYOUT_CHANGE_RESTORE,
    /** The application selected a different preferred terminal grid. */
    BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID
} boxrig_ui_layout_change_kind;

/** Input for one explicit application-originated layout change. */
typedef struct boxrig_ui_layout_change_input {
    /** Cause copied to a resulting size command. */
    boxrig_ui_layout_change_kind kind;
    /** New committed cell width in fixed-point output pixels. */
    boxrig_ui_fixed cell_width;
    /** New committed cell height in fixed-point output pixels. */
    boxrig_ui_fixed cell_height;
    /** Nonzero revision of the new committed cell metrics. */
    uint64_t cell_metrics_revision;
    /** Preferred rows for PREFERRED_GRID, otherwise zero. */
    size_t preferred_rows;
    /** Preferred columns for PREFERRED_GRID, otherwise zero. */
    size_t preferred_columns;
} boxrig_ui_layout_change_input;

/** One take-once application command for the host's content area. */
typedef struct boxrig_ui_size_command {
    /** Nonzero command revision echoed by completion. */
    uint64_t revision;
    /** Explicit application change that produced this command. */
    boxrig_ui_layout_change_kind cause;
    /** Requested content width in fixed-point output pixels. */
    boxrig_ui_fixed width;
    /** Requested content height in fixed-point output pixels. */
    boxrig_ui_fixed height;
} boxrig_ui_size_command;

/** Observable lifecycle of the current application size command. */
typedef enum boxrig_ui_size_command_state {
    /** No application size command is active. */
    BOXRIG_UI_SIZE_COMMAND_IDLE = 0,
    /** One command is available for a single host take operation. */
    BOXRIG_UI_SIZE_COMMAND_READY,
    /** The host took the command and must complete its revision. */
    BOXRIG_UI_SIZE_COMMAND_DISPATCHED
} boxrig_ui_size_command_state;

/** One effective grid change to apply to the terminal and its transport. */
typedef struct boxrig_ui_grid_resize {
    /** Nonzero monotonically increasing grid revision. */
    uint64_t revision;
    /** New terminal rows. */
    size_t rows;
    /** New terminal columns. */
    size_t columns;
} boxrig_ui_grid_resize;

/** Kind of one layout action selected from the current geometry authority. */
typedef enum boxrig_ui_layout_action_kind {
    /** Resize the application-owned terminal and transport grid. */
    BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE = 0,
    /** Request a different native or parent content extent. */
    BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE
} boxrig_ui_layout_action_kind;

/** One take-once low-level layout decision for a component host. */
typedef struct boxrig_ui_layout_action {
    /** Required external action. */
    boxrig_ui_layout_action_kind kind;
    /** Nonzero revision echoed by the matching completion operation. */
    uint64_t revision;
    /** Payload selected by kind. */
    union {
        /** Terminal and transport grid for GRID_RESIZE. */
        struct {
            /** New nonzero terminal rows. */
            size_t rows;
            /** New nonzero terminal columns. */
            size_t columns;
        } grid_resize;
        /** Native content request for CONTENT_SIZE. */
        struct {
            /** Explicit application change that produced the request. */
            boxrig_ui_layout_change_kind cause;
            /** Requested content width in fixed-point output pixels. */
            boxrig_ui_fixed width;
            /** Requested content height in fixed-point output pixels. */
            boxrig_ui_fixed height;
        } content_size;
    } value;
} boxrig_ui_layout_action;

/** Bit flags identifying portable overlays at one visual terminal cell. */
typedef enum boxrig_ui_overlay_role {
    /** Active literal-search match, drawn below all other view overlays. */
    BOXRIG_UI_OVERLAY_SEARCH_MATCH = 1u << 0,
    /** Directional text selection, drawn above a search match. */
    BOXRIG_UI_OVERLAY_SELECTION = 1u << 1,
    /** Hovered OSC 8 hyperlink, drawn above selection. */
    BOXRIG_UI_OVERLAY_HYPERLINK_HOVER = 1u << 2,
    /** Terminal cursor, drawn above view-owned overlays. */
    BOXRIG_UI_OVERLAY_CURSOR = 1u << 3,
    /** Transient IME preedit, drawn above and suppressing the cursor. */
    BOXRIG_UI_OVERLAY_IME = 1u << 4,
    /** Noninteractive input-method candidate panel above terminal content. */
    BOXRIG_UI_OVERLAY_CANDIDATES = 1u << 5
} boxrig_ui_overlay_role;

/** Independently composited fields of an application text annotation. */
typedef enum boxrig_ui_text_annotation_style_field {
    /** Replace the terminal cell underline style. */
    BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE = 1u << 0,
    /** Replace the terminal cell underline color. */
    BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR = 1u << 1,
    /** Replace the terminal cell foreground color. */
    BOXRIG_UI_ANNOTATION_FOREGROUND = 1u << 2,
    /** Replace the terminal cell background color. */
    BOXRIG_UI_ANNOTATION_BACKGROUND = 1u << 3
} boxrig_ui_text_annotation_style_field;

/** Partial cell rendition contributed by an application text annotation. */
typedef struct boxrig_ui_text_annotation_style {
    /** Bitwise boxrig_ui_text_annotation_style_field values. */
    uint32_t field_mask;
    /** Underline presentation when its field is selected. */
    boxrig_underline_style underline_style;
    /** Logical underline color when its field is selected. */
    boxrig_color underline_color;
    /** Logical foreground color when its field is selected. */
    boxrig_color foreground;
    /** Logical background color when its field is selected. */
    boxrig_color background;
} boxrig_ui_text_annotation_style;

/**
 * Resolves cell RGB colors and optionally constructs filter metadata.
 * @param terminal Borrowed terminal owning the logical colors.
 * @param cell Borrowed viewport cell; global reverse video is already included.
 * @param annotation Merged annotation overrides, or NULL for none.
 * @param background_opacity Effective background opacity, 0 through 255.
 * @param colors_out Receives foreground, background and underline colors.
 * @param input_out Optional canonical CELL input; NULL avoids metadata work.
 * @return A portable UI status. Outputs are unchanged on failure.
 * @remarks Applies cell inversion once, then annotation overrides. The caller
 * supplies presentation state and invokes boxrig_ui_view_resolve_colors when
 * filtering is enabled. Concealed glyphs and decorations must be suppressed
 * by the renderer after filtering, including intrinsic color glyphs.
 */
boxrig_ui_status boxrig_ui_color_resolve_cell(
    const boxrig_terminal *terminal,
    const boxrig_cell_view *cell,
    const boxrig_ui_text_annotation_style *annotation,
    uint8_t background_opacity,
    boxrig_ui_color_filter_result *colors_out,
    boxrig_ui_color_filter_input *input_out);

/**
 * Registers, replaces, or removes this view's display-color filter.
 * @param view View owned by the calling UI thread.
 * @param callback Deterministic callback, or NULL to remove filtering.
 * @param context Borrowed context, kept alive until removal or view destruction.
 * @return OK, OUT_OF_MEMORY, LIMIT_EXCEEDED, INVALID_ARGUMENT, or BUSY
 * during a filter call.
 * @remarks Failure preserves the previous registration. Success, including
 * registration of the same pointers, invalidates display colors and requests
 * a frame without invalidating text layouts. No cache is allocated while
 * unregistered. Retained colors are prepared for the current grid within
 * maximum_retained_color_bytes; resizing prepares all attached views before
 * replacing their retained state. Calls and context changes must be serialized
 * with drawing.
 */
boxrig_ui_status boxrig_ui_view_set_color_filter(
    boxrig_ui_view *view, boxrig_ui_color_filter callback, void *context);

/**
 * Announces a change to application-owned filter settings.
 * @param view View owned by the calling UI thread.
 * @return OK, INVALID_ARGUMENT, or BUSY during a filter call.
 * @remarks Discards cached filter results and requests repaint, including
 * margins and overlays. Does not invalidate glyph shapes or terminal colors.
 */
boxrig_ui_status boxrig_ui_view_invalidate_colors(boxrig_ui_view *view);

/**
 * Reads the generation of this view's display-color settings.
 * @param view View to inspect, or NULL.
 * @return Nonzero generation, or zero for NULL. Wrap discards cached results.
 */
uint64_t boxrig_ui_view_color_revision(const boxrig_ui_view *view);

/**
 * Tests whether a view has a registered color filter.
 * @param view View to inspect, or NULL for a direct terminal renderer.
 * @return True when filtering is enabled; false for NULL.
 * @remarks Renderers may skip metadata construction and cache lookup if false.
 */
bool boxrig_ui_view_has_color_filter(const boxrig_ui_view *view);

/**
 * Updates the keyboard focus observed by the native frontend.
 * @param view View owned by the calling UI thread.
 * @param focused Whether keyboard focus belongs to this view.
 * @return OK, INVALID_ARGUMENT, or BUSY during a color filter invocation.
 * @remarks New views start unfocused. A change requests repaint when a color
 * filter is registered, without changing terminal focus-reporting modes or
 * invalidating text layouts. X11, GTK 3/4, native Wayland and Win32 components
 * report focus automatically;
 * applications using a renderer directly must supply their own observations.
 */
boxrig_ui_status boxrig_ui_view_set_focused(boxrig_ui_view *view, bool focused);

/**
 * Reads the last frontend keyboard-focus observation.
 * @param view View to inspect, or NULL.
 * @return True when focused, otherwise false, including NULL.
 */
bool boxrig_ui_view_focused(const boxrig_ui_view *view);

/**
 * Resolves a display-color input through this view's bounded filter cache.
 * @param view View owned by the calling UI thread.
 * @param input Unfiltered display colors; unused fields are normalized.
 * @param result_out Receives only the fields used by the target on success.
 * @return OK, INVALID_ARGUMENT, or BUSY for a recursive filter invocation.
 * @remarks May synchronously invoke the registered application callback.
 * Underlines follow the returned foreground when requested. Marker results
 * cannot change their already filtered underlay. Neither concealment nor blink
 * visibility is implemented by this RGB operation; renderers enforce both.
 */
boxrig_ui_status boxrig_ui_view_resolve_colors(
    boxrig_ui_view *view,
    const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result_out);

/** One application-owned identity and style attached to logical text. */
typedef struct boxrig_ui_text_annotation {
    /** Nonzero application-assigned annotation source identity. */
    uint64_t source_id;
    /** Nonzero application-assigned item identity within the source. */
    uint64_t item_id;
    /** Half-open current range within one logical line. */
    boxrig_text_range range;
    /** Signed layer priority; greater values are composited later. */
    int32_t priority;
    /** Partial rendition contributed by this item. */
    boxrig_ui_text_annotation_style style;
} boxrig_ui_text_annotation;

/** One coalesced visual interval belonging to a text annotation item. */
typedef struct boxrig_ui_text_annotation_visual_range {
    /** Annotation whose logical range produced this interval. */
    boxrig_ui_text_annotation annotation;
    /** Visual terminal columns covered on the queried viewport row. */
    boxrig_text_layout_range visual_range;
} boxrig_ui_text_annotation_visual_range;

/** Selection policy for a pointer-authoritative annotation query. */
typedef enum boxrig_ui_text_annotation_hit_mode {
    /** Return only the item highest in the stable annotation layer order. */
    BOXRIG_UI_ANNOTATION_HIT_TOPMOST = 0,
    /** Return every intersecting item in bottom-to-top layer order. */
    BOXRIG_UI_ANNOTATION_HIT_ALL
} boxrig_ui_text_annotation_hit_mode;

/** Result of mapping a fixed-point view position to the terminal grid. */
typedef struct boxrig_ui_hit_test_result {
    /** Exact horizontal fixed-point coordinate used for this hit. */
    boxrig_ui_fixed x;
    /** Exact vertical fixed-point coordinate used for this hit. */
    boxrig_ui_fixed y;
    /** Whether the position lies inside the complete grid rectangle. */
    bool inside_grid;
    /** Zero-based row when inside_grid is true. */
    size_t row;
    /** Zero-based visual column when inside_grid is true. */
    size_t visual_column;
    /** Logical column resolved through a retained row layout. */
    size_t logical_column;
    /** View layout generation used to resolve the native coordinate. */
    uint64_t layout_generation;
    /** View annotation generation observed by this hit. */
    uint64_t annotation_generation;
    /** Whether logical text boundaries were resolved for this grid cell. */
    bool text_position_valid;
    /** Whether the hit selected the grapheme's logical trailing boundary. */
    bool trailing_half;
    /** Logical leading boundary of the complete grapheme. */
    boxrig_text_position leading_position;
    /** Logical trailing boundary of the complete grapheme. */
    boxrig_text_position trailing_position;
    /** Boundary selected by the hit half and resolved run direction. */
    boxrig_text_position position;
    /** Bitwise combination of boxrig_ui_overlay_role values at this cell. */
    uint32_t overlay_roles;
    /** Nonzero terminal-lifetime OSC 8 identity at this cell. */
    uint32_t hyperlink_handle;
    /** Borrowed untrusted UTF-8 hyperlink URI, or NULL when absent. */
    const char *hyperlink_uri;
    /** Number of bytes in hyperlink_uri. */
    size_t hyperlink_uri_length;
    /** Borrowed OSC 8 id parameter, or NULL when absent. */
    const char *hyperlink_id;
    /** Number of bytes in hyperlink_id. */
    size_t hyperlink_id_length;
} boxrig_ui_hit_test_result;

/** Unit expanded by a pointer-driven terminal text selection. */
typedef enum boxrig_ui_selection_kind {
    /** One or more complete extended grapheme clusters. */
    BOXRIG_UI_SELECTION_CHARACTER = 0,
    /** One or more Unicode default word-boundary segments. */
    BOXRIG_UI_SELECTION_WORD,
    /** One or more complete soft-wrap-joined logical lines. */
    BOXRIG_UI_SELECTION_LINE
} boxrig_ui_selection_kind;

/** Geometry used by one pointer-driven terminal text selection. */
typedef enum boxrig_ui_selection_shape {
    /** One document-order logical text range. */
    BOXRIG_UI_SELECTION_LINEAR = 0,
    /** One visual-column interval materialized independently on each row. */
    BOXRIG_UI_SELECTION_RECTANGULAR
} boxrig_ui_selection_shape;

/** Shape and expansion unit requested for a new selection. */
typedef struct boxrig_ui_selection_options {
    /** Linear or rectangular geometry. */
    boxrig_ui_selection_shape shape;
    /** Character, word, or logical-line expansion unit. */
    boxrig_ui_selection_kind kind;
} boxrig_ui_selection_options;

/** Queryable directional selection owned by one terminal view. */
typedef struct boxrig_ui_selection {
    /** Linear or rectangular geometry. */
    boxrig_ui_selection_shape shape;
    /** Unit used when extending the selection. */
    boxrig_ui_selection_kind kind;
    /** Primary or alternate logical text namespace. */
    boxrig_text_screen screen;
    /** Fixed logical boundary at the initial pointer end. */
    boxrig_text_position anchor;
    /** Logical boundary at the currently moving pointer end. */
    boxrig_text_position focus;
    /** Whether range contains a valid normalized linear selection. */
    bool range_valid;
    /** Document-order half-open range, valid only when range_valid is true. */
    boxrig_text_range range;
    /** Whether retained rectangular geometry no longer names the same text. */
    bool stale;
    /** Logical column at which the rectangular anchor's physical row begins. */
    size_t anchor_fragment_logical_column_offset;
    /** Logical column at which the rectangular focus's physical row begins. */
    size_t focus_fragment_logical_column_offset;
    /** Whether anchor_visual_row names a row in the current active viewport. */
    bool anchor_visual_row_valid;
    /** Current anchor viewport row, or zero when anchor_visual_row_valid is false. */
    size_t anchor_visual_row;
    /** Visual boundary selected at the original pointer end. */
    size_t anchor_visual_column;
    /** Whether focus_visual_row names a row in the current active viewport. */
    bool focus_visual_row_valid;
    /** Current focus viewport row, or zero when focus_visual_row_valid is false. */
    size_t focus_visual_row;
    /** Visual boundary selected at the moving pointer end. */
    size_t focus_visual_column;
    /** Whether the rectangle's first physical row is in the active viewport. */
    bool visual_start_row_valid;
    /** Current first viewport row, or zero when visual_start_row_valid is false. */
    size_t visual_start_row;
    /** Leftmost requested visual boundary before per-row grapheme expansion. */
    size_t visual_start_column;
    /** Requested visual width before per-row grapheme expansion. */
    size_t visual_column_count;
    /** View layout generation fixed by the initial pointer hit. */
    uint64_t layout_generation;
    /** Terminal text revision represented by the current boundaries. */
    uint64_t text_revision;
    /** Number of retained rectangular row slices, or zero for linear or empty. */
    size_t row_count;
    /** Number of retained rectangular logical ranges, or zero for linear or empty. */
    size_t range_count;
} boxrig_ui_selection;

/** Retained rectangular text geometry with query-time viewport coordinates. */
typedef struct boxrig_ui_selection_row_slice {
    /** Logical text namespace containing this physical fragment. */
    boxrig_text_screen screen;
    /** Stable logical line containing this physical fragment. */
    boxrig_text_line_id line_id;
    /** Required content revision of line_id. */
    uint64_t line_revision;
    /** Logical line column at which this physical fragment begins. */
    size_t fragment_logical_column_offset;
    /** Whether this physical fragment continues into the next fragment. */
    bool fragment_soft_wrapped;
    /** Top-to-bottom sequence within the materialized rectangle. */
    size_t row_sequence;
    /** Whether this physical row currently belongs to the active viewport. */
    bool visual_row_valid;
    /** Current viewport row, or zero when visual_row_valid is false. */
    size_t visual_row;
    /** Left requested visual boundary before grapheme expansion. */
    size_t requested_visual_column;
    /** Requested visual width before grapheme expansion. */
    size_t requested_visual_column_count;
    /** Left effective visual boundary after grapheme expansion. */
    size_t visual_column;
    /** Effective visual width after grapheme expansion. */
    size_t visual_column_count;
    /** First item in the selection row-range query for this row. */
    size_t range_offset;
    /** Number of stable logical ranges belonging to this row. */
    size_t range_count;
    /** Selected erased or otherwise empty display columns. */
    size_t blank_column_count;
} boxrig_ui_selection_row_slice;

/** One logical range belonging to a materialized rectangular row. */
typedef boxrig_text_snapshot_row_range boxrig_ui_selection_row_range;

/** Queryable OSC 8 hyperlink hover owned by one terminal view. */
typedef struct boxrig_ui_hyperlink_hover {
    /** Nonzero terminal-lifetime hyperlink identity. */
    uint32_t handle;
    /** Logical position of the cell that established the hover. */
    boxrig_text_position position;
    /** Borrowed untrusted UTF-8 URI, valid until terminal mutation. */
    const char *uri;
    /** Number of bytes in uri. */
    size_t uri_length;
    /** Borrowed OSC 8 id parameter, valid until terminal mutation. */
    const char *id;
    /** Number of bytes in id. */
    size_t id_length;
} boxrig_ui_hyperlink_hover;

/** Requested transparency behavior. */
typedef enum boxrig_ui_transparency_request {
    /** Require an opaque view and native toplevel. */
    BOXRIG_UI_TRANSPARENCY_OPAQUE = 0,
    /** Use alpha where every requested scope supports it. */
    BOXRIG_UI_TRANSPARENCY_PREFERRED,
    /** Report unavailable unless every requested scope supports alpha. */
    BOXRIG_UI_TRANSPARENCY_REQUIRED
} boxrig_ui_transparency_request;

/** Scope to which transparency is requested or applied. */
typedef enum boxrig_ui_transparency_scope {
    /** The renderer-owned terminal view surface. */
    BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE = 1u << 0,
    /** The platform frontend's native toplevel. */
    BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL = 1u << 1
} boxrig_ui_transparency_scope;

/** Application-owned background-transparency request. */
typedef struct boxrig_ui_transparency_config {
    /** Opaque, preferred, or required transparency policy. */
    boxrig_ui_transparency_request request;
    /** Bitwise combination of requested boxrig_ui_transparency_scope values. */
    uint32_t requested_scopes;
    /** Requested background alpha from zero through 255. */
    uint8_t background_opacity;
} boxrig_ui_transparency_config;

/** Unassociated sRGB color with independently authored eight-bit opacity. */
typedef struct boxrig_ui_rgba {
    /** Red, zero through 255. */
    uint8_t red;
    /** Green, zero through 255. */
    uint8_t green;
    /** Blue, zero through 255. */
    uint8_t blue;
    /** Opacity, zero for transparent and 255 for opaque. */
    uint8_t alpha;
} boxrig_ui_rgba;

/**
 * Sets a solid application layer behind the rendered terminal.
 * @param view View owning the copied color.
 * @param color Numeric R, G, B, A components; initially all zero.
 * @return BOXRIG_UI_OK, or BOXRIG_UI_INVALID_ARGUMENT for NULL view.
 * @remarks Terminal pixels are source-over composited onto this layer.
 * Terminal background opacity and color filters do not alter this color.
 * Alpha zero removes the layer. Changes request presentation without shaping
 * text again. This layer never changes terminal cells or Kitty images.
 */
boxrig_ui_status boxrig_ui_view_set_background_color(
    boxrig_ui_view *view, boxrig_ui_rgba color);

/**
 * Reads the application background color.
 * @param view View to inspect, or NULL for transparent black.
 * @return Copied, non-premultiplied sRGB components.
 */
boxrig_ui_rgba boxrig_ui_view_get_background_color(const boxrig_ui_view *view);

/** Centered placement within the renderer's pixel viewport. */
typedef enum boxrig_ui_background_fit {
    /** Preserve aspect ratio, cropping to fill the viewport. */
    BOXRIG_UI_BACKGROUND_COVER,
    /** Preserve aspect ratio, leaving transparent margins. */
    BOXRIG_UI_BACKGROUND_CONTAIN,
    /** Independently scale both axes to fill the viewport. */
    BOXRIG_UI_BACKGROUND_STRETCH
} boxrig_ui_background_fit;

/** Application-provided, unassociated sRGB RGBA8 pixels. */
typedef struct boxrig_ui_background_image {
    /** Width, at most 16384 pixels. */
    size_t width;
    /** Height, at most 16384 pixels. */
    size_t height;
    /** Byte distance between rows, at least width times four. */
    size_t stride;
    /** Available bytes at rgba, including any row padding. */
    size_t length;
    /** Borrowed input; copied before the setter returns. */
    const uint8_t *rgba;
    /** Overall opacity, multiplied once with each pixel's alpha. */
    uint8_t opacity;
    /** Viewport placement. */
    boxrig_ui_background_fit fit;
} boxrig_ui_background_image;

/** Renderer-facing snapshot, invalidated by image replacement or view destruction. */
typedef struct boxrig_ui_background_image_view {
    /** Pixel width; rows are tightly packed. */
    size_t width;
    /** Pixel height. */
    size_t height;
    /** Borrowed, immutable native-endian premultiplied 0xAARRGGBB pixels. */
    const uint32_t *pixels;
    /** Viewport placement. */
    boxrig_ui_background_fit fit;
    /** Revision changes on replacement and removal. */
    uint64_t revision;
} boxrig_ui_background_image_view;

/** Signed placement, permitting cover to extend outside the viewport. */
typedef struct boxrig_ui_background_rectangle {
    /** Left pixel. */
    int64_t x;
    /** Top pixel. */
    int64_t y;
    /** Scaled width in pixels. */
    size_t width;
    /** Scaled height in pixels. */
    size_t height;
} boxrig_ui_background_rectangle;

/**
 * Copies an image behind the solid background and terminal layers.
 * @param view Owner of the copy.
 * @param image Input pixels, or NULL to remove the image.
 * @return OK, INVALID_ARGUMENT, LIMIT_EXCEEDED, OUT_OF_MEMORY, or BUSY from a color callback.
 * @remarks Failure preserves the previous image. Kitty operations and scrolling
 * do not affect this layer. Successful changes request a new frame.
 */
boxrig_ui_status boxrig_ui_view_set_background_image(boxrig_ui_view *view,
    const boxrig_ui_background_image *image);

/**
 * Borrows the current image until its next replacement or view destruction.
 * @param view Owner, or NULL for an empty snapshot.
 * @return Immutable renderer-facing pixels and revision.
 */
boxrig_ui_background_image_view boxrig_ui_view_get_background_image(const boxrig_ui_view *view);

/**
 * Resolves centered pixel placement with aspect-preserving integer rounding.
 * @param image Image snapshot.
 * @param width Viewport pixel width, at most INT32_MAX.
 * @param height Viewport pixel height, at most INT32_MAX.
 * @return Placement; an empty rectangle for invalid or empty input.
 */
boxrig_ui_background_rectangle boxrig_ui_background_image_place(
    boxrig_ui_background_image_view image, size_t width, size_t height);

/** Alpha capabilities reported without exposing native platform types. */
typedef struct boxrig_ui_transparency_capabilities {
    /** Whether the frontend accepts native-endian premultiplied ARGB32. */
    bool premultiplied_argb32_target;
    /** Structurally supported boxrig_ui_transparency_scope values. */
    uint32_t supported_scopes;
    /** Currently active boxrig_ui_transparency_scope values. */
    uint32_t active_scopes;
    /** Supported scopes needing native resource recreation before activation. */
    uint32_t recreate_required_scopes;
} boxrig_ui_transparency_capabilities;

/** Effective alpha state after applying frontend capabilities. */
typedef enum boxrig_ui_transparency_effective {
    BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE = 0,
    BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA,
    BOXRIG_UI_TRANSPARENCY_UNAVAILABLE
} boxrig_ui_transparency_effective;

/** Reason an alpha request fell back or became unavailable. */
typedef enum boxrig_ui_transparency_reason {
    /** The complete request is effective, or transparency was not requested. */
    BOXRIG_UI_TRANSPARENCY_REASON_NONE = 0,
    /** A premultiplied ARGB32 render target cannot be presented. */
    BOXRIG_UI_TRANSPARENCY_REASON_ARGB32_TARGET_UNAVAILABLE,
    /** At least one requested scope is structurally unsupported. */
    BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_UNSUPPORTED,
    /** At least one supported requested scope is not currently active. */
    BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE,
    /** At least one requested scope needs explicit native resource recreation. */
    BOXRIG_UI_TRANSPARENCY_REASON_RESOURCE_RECREATION_REQUIRED
} boxrig_ui_transparency_reason;

/** Queryable requested, capable, and effective transparency state. */
typedef struct boxrig_ui_transparency_state {
    /** Current application-owned request. */
    boxrig_ui_transparency_config config;
    /** Last reported frontend capabilities. */
    boxrig_ui_transparency_capabilities capabilities;
    /** Effective portable state. */
    boxrig_ui_transparency_effective effective;
    /** Requested scopes currently using premultiplied alpha. */
    uint32_t effective_scopes;
    /** Effective background alpha, forced to 255 while opaque. */
    uint8_t effective_background_opacity;
    /** Reason for fallback or unavailability. */
    boxrig_ui_transparency_reason reason;
    /** Requested scopes that need caller-controlled resource recreation. */
    uint32_t recreate_required_scopes;
    /** Nonzero revision incremented whenever the resolved state changes. */
    uint64_t revision;
} boxrig_ui_transparency_state;

/** Portable software render-target pixel format. */
typedef enum boxrig_ui_render_target_format {
    /** Native-endian 0xAARRGGBB pixels whose alpha is always 255. */
    BOXRIG_UI_TARGET_OPAQUE_ARGB32 = 0,
    /** Native-endian premultiplied 0xAARRGGBB pixels. */
    BOXRIG_UI_TARGET_PREMULTIPLIED_ARGB32
} boxrig_ui_render_target_format;

/** Borrowed application or frontend-owned software render target. */
typedef struct boxrig_ui_render_target {
    /** First writable pixel byte, owned by the caller. */
    void *pixels;
    /** Pixel width. */
    size_t width;
    /** Pixel height. */
    size_t height;
    /** Bytes between successive rows. */
    size_t stride_bytes;
    /** Pixel format. */
    boxrig_ui_render_target_format format;
    /** Caller-defined nonzero target generation. */
    uint64_t generation;
} boxrig_ui_render_target;

/**
 * Returns conservative defaults for a terminal view.
 *
 * @return A configuration retaining at most 8 MiB of row-layout estimates,
 * 16 MiB of copied UTF-8 text, and 65,536 application annotations.
 */
boxrig_ui_view_config boxrig_ui_view_config_default(void);

/**
 * Returns the opaque background-transparency defaults.
 *
 * @return Opaque policy, both built-in scopes, and background alpha 255.
 */
boxrig_ui_transparency_config boxrig_ui_transparency_config_default(void);

/**
 * Resolves an application request against one frontend capability snapshot.
 *
 * @param config Application-owned transparency configuration.
 * @param capabilities Frontend-owned target and scope capabilities.
 * @param previous Previous resolved state for revision tracking, or NULL.
 * @param state_out Receives the complete resolved state.
 * @return BOXRIG_UI_UNAVAILABLE for an unmet required request, otherwise a
 * portable UI status.
 *
 * @remarks This function has no side effects. A valid result is written for
 * both BOXRIG_UI_OK and BOXRIG_UI_UNAVAILABLE. The caller decides whether to
 * commit that result transactionally.
 */
boxrig_ui_status boxrig_ui_transparency_resolve(
    const boxrig_ui_transparency_config *config,
    const boxrig_ui_transparency_capabilities *capabilities,
    const boxrig_ui_transparency_state *previous,
    boxrig_ui_transparency_state *state_out);

/**
 * Creates a shared dirty distributor for one borrowed terminal.
 *
 * @param terminal Terminal that outlives the model and all of its views.
 * @param model_out Receives the allocated model.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_model_create(
    boxrig_terminal *terminal,
    boxrig_ui_model **model_out);

/**
 * Releases a shared model after its views have been released.
 *
 * @param model Model to release, or NULL.
 */
void boxrig_ui_model_destroy(boxrig_ui_model *model);

/**
 * Returns the terminal borrowed by a shared model.
 *
 * @param model Model to inspect.
 * @return Borrowed terminal, or NULL.
 */
boxrig_terminal *boxrig_ui_model_terminal(boxrig_ui_model *model);

/**
 * Distributes pending core model and image dirty to every current view.
 *
 * @param model Model to synchronize after terminal mutation.
 * @return A portable UI status.
 *
 * @remarks Synchronization is explicit, never calls application code while
 * libboxrig is parsing, and lets each view consume the same update once.
 */
boxrig_ui_status boxrig_ui_model_synchronize(boxrig_ui_model *model);

/**
 * Reads the last synchronized scrollbar state without calling application code.
 * @param view View of the shared terminal viewport.
 * @param state_out Receives a value copy, including its shared model revision.
 * @return A portable UI status.
 * @remarks After direct terminal mutations, synchronize the UI model as usual.
 * In the scheduled frame, compare this revision before updating an external
 * scrollbar. History-only changes also request a frame. This getter does not
 * acknowledge dirty, move the viewport, or trigger another frame request.
 */
boxrig_ui_status boxrig_ui_view_get_scrollbar_state(
    const boxrig_ui_view *view, boxrig_scrollbar_state *state_out);

/**
 * Applies a shared viewport operation and synchronizes its UI model if changed.
 * @param view View whose shared terminal viewport is controlled.
 * @param action Portable scrollbar operation.
 * @param position Oldest-to-live position, used only for POSITION.
 * @param changed_out Receives whether the terminal viewport changed, including
 * when subsequent model synchronization returns an error.
 * @return A portable UI status; a synchronization error may follow a move.
 * @remarks A repeated position, absent history, or an alternate screen is a
 * no-op and does not advance the revision or request another frame. Other
 * views of the same model observe the same position and revision. Frame
 * callbacks schedule application-loop dispatch; they must not render inline.
 */
boxrig_ui_status boxrig_ui_view_apply_scrollbar_action(
    boxrig_ui_view *view, boxrig_scrollbar_action action,
    size_t position, bool *changed_out);

/**
 * Reads the canonical cell metrics committed by the layout authority.
 *
 * @param model Model to inspect.
 * @param set_out Receives whether canonical metrics have been committed.
 * @param metrics_out Receives the metrics when set_out is true.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_model_get_canonical_cell_metrics(
    const boxrig_ui_model *model,
    bool *set_out,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Creates the sole host-item queue consumer for a model.
 *
 * @param model Model that outlives the consumer.
 * @param consumer_out Receives the allocated consumer.
 * @return BOXRIG_UI_UNAVAILABLE while another consumer exists, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_ui_host_item_consumer_create(
    boxrig_ui_model *model,
    boxrig_ui_host_item_consumer **consumer_out);

/**
 * Releases a model host-item consumer without consuming queued items.
 *
 * @param consumer Consumer to release, or NULL.
 */
void boxrig_ui_host_item_consumer_destroy(
    boxrig_ui_host_item_consumer *consumer);

/**
 * Returns the terminal borrowed through a host-item consumer.
 *
 * @param consumer Consumer to inspect.
 * @return Borrowed terminal, or NULL after invalid destruction order.
 */
boxrig_terminal *boxrig_ui_host_item_consumer_terminal(
    boxrig_ui_host_item_consumer *consumer);

/**
 * Reads the next model-level host item without removing it.
 *
 * @param consumer Exclusive consumer to inspect.
 * @param item_out Receives a terminal-owned item view.
 * @return True when an item was returned.
 */
bool boxrig_ui_host_item_consumer_peek(
    boxrig_ui_host_item_consumer *consumer,
    boxrig_host_item *item_out);

/**
 * Removes the item most recently returned by the exclusive consumer.
 *
 * @param consumer Consumer to update.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_host_item_consumer_consume(
    boxrig_ui_host_item_consumer *consumer);

/**
 * Completes one host request through the exclusive model consumer.
 *
 * @param consumer Consumer to update.
 * @param request_id Nonzero request identifier from a host item.
 * @param result Host request result.
 * @param payload Optional response bytes.
 * @param payload_length Number of response bytes.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_host_item_consumer_complete(
    boxrig_ui_host_item_consumer *consumer,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length);

/**
 * Creates one independently consuming view of a shared terminal model.
 *
 * @param model Shared model that outlives the view.
 * @param config Bounded view configuration.
 * @param view_out Receives the allocated view.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_create(
    boxrig_ui_model *model,
    const boxrig_ui_view_config *config,
    boxrig_ui_view **view_out);

/**
 * Releases retained layouts, dirty, and target borrowing for one view.
 *
 * @param view View to release, or NULL.
 */
void boxrig_ui_view_destroy(boxrig_ui_view *view);

/**
 * Copies one input-method candidate snapshot independently of terminal text.
 * @param view View owning the bounded copy.
 * @param snapshot Borrowed data to validate and copy during this call.
 * @return OK on success; invalid data or allocation failure preserves old data.
 * @remarks Updates overlay dirty. Supporting input adapters must clear the
 * snapshot on focus loss and advertise candidate ownership only with a renderer.
 */
boxrig_ui_status boxrig_ui_view_set_candidates(boxrig_ui_view *view,
    const boxrig_ui_candidate_snapshot *snapshot);

/**
 * Removes candidate content and geometry without committing text.
 * @param view View to clear; NULL is a no-op.
 * @remarks Previously covered terminal cells are invalidated for repaint.
 */
void boxrig_ui_view_clear_candidates(boxrig_ui_view *view);

/**
 * Reads borrowed candidate content and the renderer's latest placement.
 * @param view View to inspect.
 * @param candidates_out Receives data valid until the next update or destruction.
 * @return OK, or INVALID_ARGUMENT for a NULL argument.
 * @remarks Layout revision may precede content revision until the next frame.
 */
boxrig_ui_status boxrig_ui_view_get_candidates(const boxrig_ui_view *view,
    boxrig_ui_candidate_view *candidates_out);

/**
 * Returns the shared model borrowed by a view.
 *
 * @param view View to inspect.
 * @return Borrowed model, or NULL after invalid destruction order.
 */
boxrig_ui_model *boxrig_ui_view_model(boxrig_ui_view *view);

/**
 * Returns the terminal borrowed through a view's shared model.
 *
 * @param view View to inspect.
 * @return Borrowed terminal, or NULL after invalid destruction order.
 */
boxrig_terminal *boxrig_ui_view_terminal(boxrig_ui_view *view);

/**
 * Acquires the sole canonical layout authority for a model.
 *
 * @param view View becoming the authority.
 * @return BOXRIG_UI_UNAVAILABLE while another view is authoritative,
 * otherwise a portable UI status.
 */
boxrig_ui_status boxrig_ui_view_acquire_layout_authority(
    boxrig_ui_view *view);

/**
 * Releases canonical layout authority without changing terminal geometry.
 *
 * @param view View that may currently be authoritative, or NULL.
 */
void boxrig_ui_view_release_layout_authority(boxrig_ui_view *view);

/**
 * Reports whether a view owns its model's canonical layout authority.
 *
 * @param view View to inspect.
 * @return True only for the current authority.
 */
bool boxrig_ui_view_is_layout_authority(const boxrig_ui_view *view);

/**
 * Validates effective metrics against canonical model ownership.
 *
 * @param view Authority or presentation-only view to validate.
 * @param metrics Candidate effective metrics.
 * @return BOXRIG_UI_UNAVAILABLE for unsupported heterogeneous metrics,
 * otherwise a portable UI status.
 */
boxrig_ui_status boxrig_ui_view_validate_cell_metrics(
    const boxrig_ui_view *view,
    const boxrig_font_effective_metrics *metrics);

/**
 * Commits canonical cell metrics and terminal protocol geometry.
 *
 * @param view Current layout authority.
 * @param metrics Prevalidated effective metrics.
 * @return A portable UI status.
 *
 * @remarks The terminal, model, and view are borrowed. This operation does not
 * resize the terminal grid or any native window.
 */
boxrig_ui_status boxrig_ui_view_commit_cell_metrics(
    boxrig_ui_view *view,
    const boxrig_font_effective_metrics *metrics);

/**
 * Installs an application-loop frame request notification.
 *
 * @param view View to configure.
 * @param callback Callback for future frame requests, or NULL.
 * @param context Opaque callback context, which must be NULL without callback.
 * @return A portable UI status.
 *
 * @remarks The callback requests dispatch only; rendering remains owned by the
 * application event loop. Existing dirty is not synchronously reported while
 * installing the callback. A clean-to-dirty transition requests a frame;
 * ending synchronized output also requests any retained, visible dirty frame.
 */
boxrig_ui_status boxrig_ui_view_set_frame_request_callback(
    boxrig_ui_view *view,
    boxrig_ui_frame_request_callback callback,
    void *context);

/**
 * Updates whether a frontend can currently present this view.
 *
 * @param view View to configure.
 * @param visible Whether frame requests may be delivered.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_set_visible(
    boxrig_ui_view *view,
    bool visible);

/**
 * Reports whether any dirty still requires a frame.
 *
 * @param view View to inspect.
 * @return True while one or more retained dirty layers are nonempty.
 */
bool boxrig_ui_view_frame_requested(const boxrig_ui_view *view);

/**
 * Reads the next retained rectangle without consuming it.
 *
 * @param view View to inspect.
 * @param layer Dirty plane to inspect.
 * @param dirty_out Receives the next coalesced rectangle.
 * @return True when dirty was returned.
 */
bool boxrig_ui_view_peek_dirty(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    boxrig_ui_dirty *dirty_out);

/**
 * Reads the retained dirty interval for one exact row without consuming it.
 *
 * @param view View to inspect.
 * @param layer Dirty plane to inspect.
 * @param row Zero-based terminal row.
 * @param dirty_out Receives a one-row dirty interval.
 * @return True when that row is dirty.
 */
bool boxrig_ui_view_get_row_dirty(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t row,
    boxrig_ui_dirty *dirty_out);

/**
 * Consumes the rectangle currently returned for one dirty layer.
 *
 * @param view View to update.
 * @param layer Dirty plane to consume.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_consume_dirty(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer);

/**
 * Adds frontend-owned overlay or presentation dirty.
 *
 * @param view View to invalidate.
 * @param layer Overlay or presentation layer.
 * @param dirty Visual grid rectangle clipped to the current effective grid.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_invalidate(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    boxrig_ui_dirty dirty);

/**
 * Sets host geometry capabilities independently from allocation events.
 *
 * @param view View whose host capability state is replaced.
 * @param capabilities Bitwise boxrig_ui_geometry_capability values.
 * @return A portable UI status.
 *
 * @remarks An already-ready or dispatched command keeps its lifecycle. The
 * new state controls only later application layout changes.
 */
boxrig_ui_status boxrig_ui_view_set_geometry_capabilities(
    boxrig_ui_view *view,
    uint32_t capabilities);

/**
 * Reads host geometry capabilities.
 *
 * @param view View to inspect.
 * @param capabilities_out Receives bitwise geometry capabilities.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_get_geometry_capabilities(
    const boxrig_ui_view *view,
    uint32_t *capabilities_out);

/**
 * Accepts one actual allocation without producing a native size command.
 *
 * @param view View whose effective grid and normal anchor are updated.
 * @param input Actual allocation, committed metrics, and native state.
 * @param result_out Receives the effective grid and excess area.
 * @return A portable UI status.
 *
 * @remarks This operation may queue a portable terminal grid resize but can
 * never transition a size command from IDLE to READY.
 */
boxrig_ui_status boxrig_ui_view_accept_allocation(
    boxrig_ui_view *view,
    const boxrig_ui_allocation_input *input,
    boxrig_ui_layout_result *result_out);

/**
 * Applies one explicit application-originated layout change.
 *
 * @param view Current layout authority.
 * @param input New metrics and an explicit application cause.
 * @param result_out Receives the current effective layout.
 * @return A portable UI status.
 *
 * @remarks PREFERRED_GRID requires nonzero preferred rows and columns. Other
 * causes require both preferred fields to be zero. Only this operation and
 * boxrig_ui_view_set_preferred_grid() may create a size command.
 */
boxrig_ui_status boxrig_ui_view_apply_layout_change(
    boxrig_ui_view *view,
    const boxrig_ui_layout_change_input *input,
    boxrig_ui_layout_result *result_out);

/**
 * Reads the current size-command lifecycle without exposing its payload.
 *
 * @param view View to inspect.
 * @param state_out Receives IDLE, READY, or DISPATCHED.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_get_size_command_state(
    const boxrig_ui_view *view,
    boxrig_ui_size_command_state *state_out);

/**
 * Completes one dispatched application size command.
 *
 * @param view View containing the command.
 * @param revision Dispatched command revision.
 * @param outcome Accepted, constrained, rejected, or superseded result.
 * @param allocation_width Actual content width after the host decision.
 * @param allocation_height Actual content height after the host decision.
 * @param applied_out Receives false for a stale or undispatched revision.
 * @param result_out Receives the current effective result.
 * @return A portable UI status.
 *
 * @remarks Completion never creates another size command. Stale completion
 * does not inspect or apply the supplied allocation.
 */
boxrig_ui_status boxrig_ui_view_complete_size_command(
    boxrig_ui_view *view,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *result_out);

/**
 * Reads the last effective allocation and grid result.
 *
 * @param view View to inspect.
 * @param result_out Receives the current result.
 * @return BOXRIG_UI_INVALID_ARGUMENT before initial layout, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_ui_view_get_layout(
    const boxrig_ui_view *view,
    boxrig_ui_layout_result *result_out);

/**
 * Changes the normal-state preferred terminal grid.
 *
 * @param view Current layout authority.
 * @param rows Preferred nonzero row count.
 * @param columns Preferred nonzero column count.
 * @return BOXRIG_UI_UNAVAILABLE for a presentation-only view, otherwise a
 * portable UI status.
 *
 * @remarks A configured view may issue one take-once size command, but this
 * operation never resizes the terminal, session, parent, or toplevel.
 */
boxrig_ui_status boxrig_ui_view_set_preferred_grid(
    boxrig_ui_view *view,
    size_t rows,
    size_t columns);

/**
 * Reads the normal-state preferred terminal grid.
 *
 * @param view View to inspect.
 * @param rows_out Receives preferred rows.
 * @param columns_out Receives preferred columns.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_get_preferred_grid(
    const boxrig_ui_view *view,
    size_t *rows_out,
    size_t *columns_out);

/**
 * Reads the currently executable low-level layout decision without taking it.
 *
 * @param view Layout-authority view to inspect.
 * @param action_out Receives one tagged grid or content-size action.
 * @return True when one action is currently executable.
 *
 * @remarks Repeated calls return the same revision until the action is taken
 * or a newer pending grid decision replaces it. Content-size has priority.
 */
bool boxrig_ui_view_peek_layout_action(
    const boxrig_ui_view *view,
    boxrig_ui_layout_action *action_out);

/**
 * Removes the currently executable low-level layout decision.
 *
 * @param view Layout-authority view to update.
 * @param action_out Receives one tagged grid or content-size action.
 * @return True when one action was returned.
 *
 * @remarks A content-size action has priority and prevents a grid action from
 * being exposed until its revision is completed. This view-level operation
 * does not establish a native presentation boundary. Applications using a
 * native component must use that component's layout-action operation instead.
 */
bool boxrig_ui_view_take_layout_action(
    boxrig_ui_view *view,
    boxrig_ui_layout_action *action_out);

/**
 * Retains an owned portable row layout for hit testing and IME placement.
 *
 * @param view View taking ownership on success.
 * @param row Zero-based effective grid row.
 * @param layout Allocated layout transferred only on success.
 * @param estimated_bytes Bytes charged against the configured cache limit.
 * @param revision Nonzero model or renderer layout revision.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_store_row_layout(
    boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout *layout,
    size_t estimated_bytes,
    uint64_t revision);

/**
 * Attaches a shared physical-row analyzer to a view.
 * @param view View whose selection may extend beyond the viewport.
 * @param provider Open provider to retain, or NULL to detach.
 * @return OK, INVALID_ARGUMENT, LIMIT_EXCEEDED, or BUSY during analysis.
 * @remarks The owner keeps its handle. Replacing an existing provider makes
 * rectangular selections stale. First attachment adopts any retained geometry;
 * the host must use the same display policy that produced that geometry.
 */
boxrig_ui_status boxrig_ui_view_set_fragment_layout_provider(
    boxrig_ui_view *view, boxrig_ui_fragment_layout_provider *provider);

/**
 * Maps one fixed-point view position through the retained row layout.
 *
 * @param view View to inspect.
 * @param x Horizontal position relative to the view content.
 * @param y Vertical position relative to the view content.
 * @param result_out Receives grid membership and cell coordinates.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_hit_test(
    boxrig_ui_view *view,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out);

/**
 * Maps a position and includes a synchronized transient composition overlay.
 *
 * @param view View to inspect.
 * @param composition Optional composition synchronized for this view.
 * @param x Horizontal position relative to view content.
 * @param y Vertical position relative to view content.
 * @param result_out Receives text, hyperlink, and complete overlay hit state.
 * @return A portable UI status.
 *
 * @remarks A visible preedit suppresses BOXRIG_UI_OVERLAY_CURSOR and reports
 * BOXRIG_UI_OVERLAY_IME for each placed cluster cell. The composition remains
 * frontend-owned and is not retained by the view.
 */
boxrig_ui_status boxrig_ui_view_hit_test_with_composition(
    boxrig_ui_view *view,
    const boxrig_composition *composition,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out);

/**
 * Atomically replaces one source's annotation set for one logical line.
 *
 * @param view View that owns the application annotations.
 * @param source_id Nonzero source identity shared by every input item.
 * @param screen Logical text namespace containing the line.
 * @param line_id Stable logical-line identity used even for an empty set.
 * @param line_revision Required current revision of line_id.
 * @param annotations Replacement items, or NULL when annotation_count is zero.
 * @param annotation_count Number of replacement items.
 * @return A portable UI status.
 *
 * @remarks Every range must be nonempty, current, and contained by the named
 * logical line. Source/item pairs are unique across the view. Validation and
 * allocation complete before retained state or overlay dirty is changed.
 */
boxrig_ui_status boxrig_ui_view_replace_text_annotations(
    boxrig_ui_view *view,
    uint64_t source_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision,
    const boxrig_ui_text_annotation *annotations,
    size_t annotation_count);

/**
 * Clears every text annotation belonging to one application source.
 *
 * @param view View to update.
 * @param source_id Nonzero source identity to clear.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_clear_text_annotations(
    boxrig_ui_view *view,
    uint64_t source_id);

/**
 * Copies current annotation intervals visible on one viewport row.
 *
 * @param view View to inspect.
 * @param row Zero-based active viewport row.
 * @param ranges Caller-owned output array, or NULL when capacity is zero.
 * @param capacity Number of output elements available.
 * @param required_count_out Receives the exact required element count.
 * @return BOXRIG_UI_OK, BOXRIG_UI_LIMIT_EXCEEDED without a partial prefix,
 * or BOXRIG_UI_INVALID_ARGUMENT.
 *
 * @remarks Items are ordered by priority, source ID, and item ID from bottom
 * to top. Each item's disjoint bidi intervals remain separate while adjacent
 * visual cells are coalesced. Stale line revisions are omitted.
 */
boxrig_ui_status boxrig_ui_view_query_text_annotation_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_ui_text_annotation_visual_range *ranges,
    size_t capacity,
    size_t *required_count_out);

/**
 * Copies annotations intersecting the exact grapheme of a prior pointer hit.
 *
 * @param view View that produced hit.
 * @param hit Revisioned result returned by boxrig_ui_view_hit_test or its
 * composition-aware variant.
 * @param mode Whether to return the topmost item or every item.
 * @param annotations Caller-owned output array, or NULL when capacity is zero.
 * @param capacity Number of annotation elements available.
 * @param required_count_out Receives the exact required element count.
 * @param merged_style_out Optional field-wise bottom-to-top style merge.
 * @return BOXRIG_UI_OK, BOXRIG_UI_LIMIT_EXCEEDED without partial output,
 * BOXRIG_UI_STALE when text, line, layout, or annotation authority changed,
 * or BOXRIG_UI_INVALID_ARGUMENT.
 *
 * @remarks BOXRIG_UI_ANNOTATION_HIT_ALL is returned in stable bottom-to-top
 * order. The query allocates no memory and uses both grapheme boundaries,
 * never only hit.position.
 */
boxrig_ui_status boxrig_ui_view_query_text_annotation_hits(
    const boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit,
    boxrig_ui_text_annotation_hit_mode mode,
    boxrig_ui_text_annotation *annotations,
    size_t capacity,
    size_t *required_count_out,
    boxrig_ui_text_annotation_style *merged_style_out);

/**
 * Starts or replaces this view's directional logical text selection.
 *
 * @param view View that owns the selection.
 * @param kind Character, Unicode word, or logical-line expansion.
 * @param hit Boundary-aware hit result produced for this active viewport.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_begin_selection(
    boxrig_ui_view *view,
    boxrig_ui_selection_kind kind,
    const boxrig_ui_hit_test_result *hit);

/**
 * Starts or replaces this view's directional shape-aware text selection.
 *
 * @param view View that owns the selection.
 * @param options Linear character/word/line or rectangular character policy.
 * @param hit Boundary-aware hit result produced for this active viewport.
 * @return BOXRIG_UI_OK, BOXRIG_UI_INVALID_ARGUMENT for an unsupported
 * shape/kind pair, BOXRIG_UI_LIMIT_EXCEEDED when materialized row or range
 * limits would be exceeded, BOXRIG_UI_OUT_OF_MEMORY, or BOXRIG_UI_STALE.
 *
 * @remarks Rectangular word and line selections are not supported. Failure is
 * transactional and preserves any prior selection. The shorthand
 * boxrig_ui_view_begin_selection always starts a linear selection. A
 * rectangular selection uses the hit cell half as a visual column boundary,
 * clamps to the active grid, and expands an edge outward rather than splitting
 * a wide or otherwise multi-cell grapheme.
 */
boxrig_ui_status boxrig_ui_view_begin_selection_with_options(
    boxrig_ui_view *view,
    const boxrig_ui_selection_options *options,
    const boxrig_ui_hit_test_result *hit);

/**
 * Extends this view's current selection to another boundary-aware hit.
 *
 * @param view View whose focus is moving.
 * @param hit Boundary-aware hit result produced for this active viewport.
 * @return BOXRIG_UI_UNAVAILABLE when no selection exists, otherwise a
 * portable UI status.
 *
 * @remarks A current hit may have a later layout generation than the initial
 * hit when deterministic row layouts were populated between pointer events.
 * Grid, cell-metric, reflow, and selected-text changes still make a retained
 * rectangular selection stale.
 * Equal rectangular column boundaries produce a successful empty selection,
 * on either the same row or another row. Its anchor remains available for
 * later extension; row and range counts are zero and it has no visual ranges.
 */
boxrig_ui_status boxrig_ui_view_update_selection(
    boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit);

/**
 * Reads this view's current directional selection.
 *
 * @param view View to inspect.
 * @param selection_out Receives anchor, focus, shape, and query counts.
 * @return True when a selection exists.
 *
 * @remarks range_valid is true only for a normalized linear range. A stale
 * rectangular selection remains observable here, while its bounded row and
 * range queries reject the retained details with BOXRIG_UI_STALE.
 * An empty rectangular selection also returns true, with zero visual width,
 * row count and range count. Clearing it explicitly removes the anchor.
 * Rectangular fragment offsets identify physical rows independently of the
 * viewport. Visual row validity flags describe the current viewport, are false
 * for stale selections, and are unused for linear selections.
 */
bool boxrig_ui_view_get_selection(
    const boxrig_ui_view *view,
    boxrig_ui_selection *selection_out);

/**
 * Copies this view's retained rectangular row descriptions without allocation.
 *
 * @param view View to inspect.
 * @param rows_out Caller-owned destination, or NULL when capacity is zero.
 * @param capacity Number of row elements available.
 * @param required_count_out Receives the exact required row count.
 * @return BOXRIG_UI_OK, BOXRIG_UI_LIMIT_EXCEEDED without partial output,
 * BOXRIG_UI_STALE, BOXRIG_UI_UNAVAILABLE for no rectangular selection, or
 * BOXRIG_UI_INVALID_ARGUMENT.
 *
 * @remarks Rows are returned from top to bottom. Requested columns describe
 * the pointer rectangle, while effective columns include whole-grapheme edge
 * expansion and blank_column_count records selected empty display cells.
 * Text identities and ranges remain stable across scrolling; visual_row and
 * visual_row_valid describe the current active viewport at query time.
 */
boxrig_ui_status boxrig_ui_view_query_selection_rows(
    const boxrig_ui_view *view,
    boxrig_ui_selection_row_slice *rows_out,
    size_t capacity,
    size_t *required_count_out);

/**
 * Copies all logical ranges of this view's rectangular rows without allocation.
 *
 * @param view View to inspect.
 * @param ranges_out Caller-owned destination, or NULL when capacity is zero.
 * @param capacity Number of range elements available.
 * @param required_count_out Receives the exact required range count.
 * @return BOXRIG_UI_OK, BOXRIG_UI_LIMIT_EXCEEDED without partial output,
 * BOXRIG_UI_STALE, BOXRIG_UI_UNAVAILABLE for no rectangular selection, or
 * BOXRIG_UI_INVALID_ARGUMENT.
 *
 * @remarks Ranges are ordered by top-to-bottom row sequence and then logical
 * document order within each row. Each range contains complete graphemes.
 */
boxrig_ui_status boxrig_ui_view_query_selection_row_ranges(
    const boxrig_ui_view *view,
    boxrig_ui_selection_row_range *ranges_out,
    size_t capacity,
    size_t *required_count_out);

/**
 * Clears this view's selection and invalidates its visible overlay.
 *
 * @param view View to update.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_clear_selection(boxrig_ui_view *view);

/**
 * Resolves this view's selected logical cells on one row to visual ranges.
 *
 * @param view View to inspect.
 * @param row Zero-based active viewport row.
 * @param ranges_out Optional destination sorted by visual column.
 * @param range_capacity Number of elements available in ranges_out.
 * @return Number of required coalesced ranges, which may exceed capacity, or
 * zero when the row is invalid or unselected.
 *
 * @remarks This is the shape-independent renderer entry point. Stale or
 * inactive-screen rectangular selections produce no visual ranges.
 */
size_t boxrig_ui_view_selection_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity);

/**
 * Starts or replaces the resumable literal-search traversal for this view.
 *
 * @param view View owning traversal and active result state.
 * @param options Core search inputs copied by the new cursor.
 * @return A portable UI status.
 *
 * @remarks An existing active result remains highlighted until a new match or
 * not-found completion replaces it. Cursor creation is transactional.
 */
boxrig_ui_status boxrig_ui_view_begin_search(
    boxrig_ui_view *view,
    const boxrig_text_search_options *options);

/**
 * Advances this view's search by a bounded integer work budget.
 *
 * @param view View whose cursor is active.
 * @param work_budget Positive maximum work for this dispatch.
 * @param work_used_out Receives work no greater than work_budget.
 * @param progress_out Receives in-progress, match, or not-found.
 * @return BOXRIG_UI_UNAVAILABLE without an active traversal, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_ui_view_resume_search(
    boxrig_ui_view *view,
    size_t work_budget,
    size_t *work_used_out,
    boxrig_text_search_progress *progress_out);

/**
 * Reads the active literal-search match owned by this view.
 *
 * @param view View to inspect.
 * @param result_out Receives the immutable result value.
 * @return True when an active result exists.
 */
bool boxrig_ui_view_get_search_result(
    const boxrig_ui_view *view,
    boxrig_text_search_result *result_out);

/**
 * Reveals the active result when it belongs to the active text namespace.
 *
 * @param view View whose terminal viewport may move.
 * @return BOXRIG_UI_UNAVAILABLE without a result or while its screen is
 * inactive, BOXRIG_UI_STALE for changed text, otherwise a portable status.
 *
 * @remarks The application must synchronize the shared model after a
 * successful viewport move before rendering or hit testing.
 */
boxrig_ui_status boxrig_ui_view_reveal_search_result(boxrig_ui_view *view);

/**
 * Clears this view's traversal and active search overlay.
 *
 * @param view View to update.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_clear_search(boxrig_ui_view *view);

/**
 * Resolves this view's active search match on one row to visual ranges.
 *
 * @param view View to inspect.
 * @param row Zero-based active viewport row.
 * @param ranges_out Optional destination sorted by visual column.
 * @param range_capacity Number of elements available in ranges_out.
 * @return Number of required coalesced ranges, possibly exceeding capacity.
 */
size_t boxrig_ui_view_search_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity);

/**
 * Replaces this view's hyperlink hover from a current hit result.
 *
 * @param view View owning transient hover state.
 * @param hit Current boundary-aware hit containing a hyperlink.
 * @return BOXRIG_UI_UNAVAILABLE when hit has no hyperlink, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_ui_view_update_hyperlink_hover(
    boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit);

/**
 * Reads this view's current borrowed hyperlink hover.
 *
 * @param view View to inspect.
 * @param hover_out Receives identity, position, URI, and id.
 * @return True while a current hovered hyperlink exists.
 */
bool boxrig_ui_view_get_hyperlink_hover(
    const boxrig_ui_view *view,
    boxrig_ui_hyperlink_hover *hover_out);

/**
 * Clears this view's hyperlink hover and invalidates its old visual ranges.
 *
 * @param view View to update.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_clear_hyperlink_hover(boxrig_ui_view *view);

/**
 * Resolves the hovered hyperlink cells on one row to visual ranges.
 *
 * @param view View to inspect.
 * @param row Zero-based active viewport row.
 * @param ranges_out Optional destination sorted by visual column.
 * @param range_capacity Number of elements available in ranges_out.
 * @return Number of required coalesced ranges, possibly exceeding capacity.
 */
size_t boxrig_ui_view_hyperlink_hover_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity);

/**
 * Creates an immutable logical-order UTF-8 copy of this view's selection.
 *
 * @param view View whose selection is copied.
 * @param snapshot_out Receives an owned snapshot and is set to NULL on
 * failure.
 * @return BOXRIG_UI_UNAVAILABLE when no selection exists or a rectangle is empty,
 * BOXRIG_UI_LIMIT_EXCEEDED without truncation when the configured byte cap is
 * exceeded, BOXRIG_UI_STALE when retained text changed, or another portable
 * UI status.
 *
 * @remarks Rectangular selections preserve empty and trailing padding columns
 * as spaces and insert one LF between every materialized visual row, including
 * soft-wrap rows, but no final LF. Each row emits complete graphemes in logical
 * document order, so a wide grapheme appears once and bidi shaping never
 * reverses its UTF-8 bytes.
 */
boxrig_ui_status boxrig_ui_view_create_selection_snapshot(
    boxrig_ui_view *view,
    boxrig_text_snapshot **snapshot_out);

/**
 * Transactionally changes the application-owned transparency request.
 *
 * @param view View to configure.
 * @param config New request to resolve against the last reported capabilities.
 * @return BOXRIG_UI_UNAVAILABLE without changing view state or its render
 * target when a required request cannot be met; otherwise a portable status.
 */
boxrig_ui_status boxrig_ui_view_set_transparency_config(
    boxrig_ui_view *view,
    const boxrig_ui_transparency_config *config);

/**
 * Reports a new frontend-owned transparency capability snapshot.
 *
 * @param view View whose frontend capabilities changed.
 * @param capabilities Complete current target and scope capabilities.
 * @return A portable UI status.
 *
 * @remarks Capability loss is committed even when a previously accepted
 * required request becomes unavailable. Such a transition is queryable and
 * invalidates the complete presentation surface.
 */
boxrig_ui_status boxrig_ui_view_report_transparency_capabilities(
    boxrig_ui_view *view,
    const boxrig_ui_transparency_capabilities *capabilities);

/**
 * Reads requested, capable, and effective transparency state.
 *
 * @param view View to inspect.
 * @param state_out Receives the complete portable state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_get_transparency(
    const boxrig_ui_view *view,
    boxrig_ui_transparency_state *state_out);

/**
 * Borrows a caller-owned software target until detach or view destruction.
 *
 * @param view View that will draw into the target.
 * @param target Valid target whose pixel storage remains caller-owned.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_attach_render_target(
    boxrig_ui_view *view,
    const boxrig_ui_render_target *target);

/**
 * Ends render-target borrowing without discarding terminal updates.
 *
 * @param view View to detach.
 * @param target_lost Whether retained presentation content was lost.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_ui_view_detach_render_target(
    boxrig_ui_view *view,
    bool target_lost);

/**
 * Reads the currently borrowed software target.
 *
 * @param view View to inspect.
 * @param target_out Receives a borrowed descriptor.
 * @return True while a target is attached.
 */
bool boxrig_ui_view_get_render_target(
    const boxrig_ui_view *view,
    boxrig_ui_render_target *target_out);

/**
 * Atomically changes the view's text and cursor blink policy.
 * @param view View to configure.
 * @param config Positive durations and independent enable flags.
 * @return Portable status. Invalid configuration or scheduling failure leaves
 * the previous configuration, phase, and timer reservation unchanged.
 * @remarks Effective changes restart the corresponding enabled phase at its
 * visible interval. Terminal attributes and display-color revisions stay
 * unchanged. Connected components manage timers; standalone renderers can
 * use the lower-level time-update and deadline APIs.
 */
boxrig_ui_status boxrig_ui_view_set_blink_config(
    boxrig_ui_view *view, const boxrig_ui_blink_config *config);

/**
 * Reads the current view-local blinking policy.
 * @param view View to inspect.
 * @param config_out Receives a copied policy on success.
 * @return OK or INVALID_ARGUMENT; failure leaves the output unchanged.
 */
boxrig_ui_status boxrig_ui_view_get_blink_config(
    const boxrig_ui_view *view, boxrig_ui_blink_config *config_out);

/**
 * Supplies monotonic milliseconds to an unattached view.
 * @param view Standalone view to advance.
 * @param milliseconds Absolute time in one monotonic domain.
 * @return OK, INVALID_ARGUMENT for rollback, or BUSY while runtime-managed.
 * @remarks The first sample establishes visible phase origins. Equal time is
 * harmless; long pauses skip directly to the current phase. This drives only
 * view blinking, not the terminal's independent image clock.
 */
boxrig_ui_status boxrig_ui_view_update_monotonic_time(
    boxrig_ui_view *view, uint64_t milliseconds);

/**
 * Reads the next view blink transition without reading a clock.
 * @param view View to inspect.
 * @param deadline_out Receives absolute milliseconds when a deadline exists.
 * @return True for a future representable transition, otherwise false.
 * @remarks Hidden views, absent targets, disabled blinking, and suppressed or
 * unfocused cursors contribute no cursor deadline. Images are queried from
 * the terminal or aggregated automatically by a connected runtime.
 */
bool boxrig_ui_view_next_animation_deadline(
    const boxrig_ui_view *view, uint64_t *deadline_out);

/**
 * Copies one view's insertion-cursor presentation style.
 * @param view View to change on its owning UI thread.
 * @param style Copied style; no pointers are borrowed.
 * @return OK, INVALID_ARGUMENT for invalid policies or box/stroke widths, BUSY
 * during a color callback, or a scheduling failure with the old style retained.
 * @remarks Terminal visibility and IME suppression are never overridden.
 * Intervals and global enablement come from the view's blink configuration.
 */
boxrig_ui_status boxrig_ui_view_set_cursor_style(
    boxrig_ui_view *view, const boxrig_ui_cursor_style *style);

/**
 * Reads the copied cursor style without changing terminal state.
 * @param view View to inspect.
 * @param style_out Receives the configured style, including follow policies.
 * @return OK or INVALID_ARGUMENT for a NULL argument.
 */
boxrig_ui_status boxrig_ui_view_get_cursor_style(
    const boxrig_ui_view *view, boxrig_ui_cursor_style *style_out);

/**
 * Restores terminal-following cursor policies and a one-DIP stroke.
 * @param view View to change on its owning UI thread.
 * @return The status of applying boxrig_ui_cursor_style_default().
 */
boxrig_ui_status boxrig_ui_view_reset_cursor_style(boxrig_ui_view *view);

/**
 * Copies selection decoration into this view without changing selected text.
 * @param view View to change on its owning UI thread.
 * @param style Complete style; positive finite DIP width and valid enums required.
 * @return OK, INVALID_ARGUMENT, or BUSY during a color-filter callback.
 * @remarks Invalid settings preserve the old style. Changed settings redraw
 * only the existing selection, without invalidating text shaping or glyphs.
 * Focus loss preserves this style; alpha is independent of terminal opacity.
 */
boxrig_ui_status boxrig_ui_view_set_selection_style(
    boxrig_ui_view *view, const boxrig_ui_selection_style *style);

/**
 * Reads a copy of this view's selection decoration.
 * @param view View to inspect.
 * @param style_out Receives the complete style only on success.
 * @return OK or INVALID_ARGUMENT for a NULL argument.
 */
boxrig_ui_status boxrig_ui_view_get_selection_style(
    const boxrig_ui_view *view, boxrig_ui_selection_style *style_out);

/**
 * Restores the default blue outline and original selected glyph colors.
 * @param view View to change on its owning UI thread.
 * @return The status of applying boxrig_ui_selection_style_default().
 */
boxrig_ui_status boxrig_ui_view_reset_selection_style(boxrig_ui_view *view);

/**
 * Reads the cursor's view-local blink visibility.
 * @param view View to inspect.
 * @return True for visible or steady phase; false for the hidden phase.
 * @remarks Renderers still apply logical cursor visibility, viewport, and IME
 * suppression. This presentation gate never changes color-filter inputs.
 */
bool boxrig_ui_view_cursor_blink_visible(const boxrig_ui_view *view);

/**
 * Reads the text blink phase selected by a cell's logical attributes.
 * @param view View to inspect, or NULL for steady standalone rendering.
 * @param attributes Bitwise combination of boxrig_cell_attribute values.
 * @return True for steady or visible text; false for the hidden blink phase.
 * @remarks Apply this gate to glyphs, underlines and strikethrough only.
 * Backgrounds, images, selection/search/hover markers and cursors remain
 * independent. Renderers must additionally suppress concealed text.
 */
bool boxrig_ui_view_text_blink_visible(const boxrig_ui_view *view,
    uint32_t attributes);

/**
 * Restarts only the cursor phase after a local input action.
 * @param view View receiving the input.
 * @return A portable status. Scheduling failure preserves the previous phase.
 * @remarks Native input adapters call this when accepting local keyboard,
 * committed text, or paste input. Text blink origins are not reset.
 */
boxrig_ui_status boxrig_ui_view_notify_local_input(boxrig_ui_view *view);

/**
 * Suspends cursor blinking while an IME composition replaces the cursor.
 * @param view View containing the composition.
 * @param active Whether the composition currently suppresses the cursor.
 * @return A portable status; scheduling failure preserves the old state.
 * @remarks Native components synchronize this at composition changes.
 * Standalone renderers notify it when changing their composition input.
 */
boxrig_ui_status boxrig_ui_view_set_composition_active(
    boxrig_ui_view *view, bool active);

#ifdef __cplusplus
}
#endif

#endif
