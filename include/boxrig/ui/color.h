/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_COLOR_H
#define BOXRIG_UI_COLOR_H

#include "boxrig/terminal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Display operation whose colors are being resolved. */
typedef enum boxrig_ui_color_target {
    /** Terminal cell, including a blank cell. */
    BOXRIG_UI_COLOR_CELL = 0,
    /** Clear color and margins outside the grid. */
    BOXRIG_UI_COLOR_SURFACE,
    /** Terminal insertion cursor. */
    BOXRIG_UI_COLOR_CURSOR,
    /** Copy-selection outline. */
    BOXRIG_UI_COLOR_SELECTION_MARKER,
    /** Search-result outline. */
    BOXRIG_UI_COLOR_SEARCH_MARKER,
    /** Hovered hyperlink decoration. */
    BOXRIG_UI_COLOR_HOVER_MARKER,
    /** IME preedit text and background. */
    BOXRIG_UI_COLOR_IME,
    /** IME insertion or candidate-position marker. */
    BOXRIG_UI_COLOR_IME_MARKER
} boxrig_ui_color_target;

/** Origin of one displayed color, after inversion and annotation overrides. */
typedef enum boxrig_ui_color_source_role {
    /** This color is unused by the operation. */
    BOXRIG_UI_COLOR_UNUSED = 0,
    /** Terminal default foreground, including OSC 10 changes. */
    BOXRIG_UI_COLOR_DEFAULT_FOREGROUND,
    /** Terminal default background, including OSC 11 changes. */
    BOXRIG_UI_COLOR_DEFAULT_BACKGROUND,
    /** Terminal cursor color, including OSC 12 changes. */
    BOXRIG_UI_COLOR_DEFAULT_CURSOR,
    /** Indexed terminal palette entry. */
    BOXRIG_UI_COLOR_INDEXED,
    /** Explicit terminal RGB color. */
    BOXRIG_UI_COLOR_RGB,
    /** Frontend or view style color. */
    BOXRIG_UI_COLOR_UI,
    /** Underline inherits the displayed foreground. */
    BOXRIG_UI_COLOR_FOLLOWS_FOREGROUND
} boxrig_ui_color_source_role;

/** Logical color and its display origin; no receive history is retained. */
typedef struct boxrig_ui_color_source {
    /** Logical value. Unused components are normalized to zero. */
    boxrig_color logical;
    /** Distinguishes default roles even after foreground/background inversion. */
    boxrig_ui_color_source_role role;
    /** Whether a text annotation supplied this value. */
    bool annotated;
} boxrig_ui_color_source;

/** Concurrent presentation conditions; blinking phase is deliberately absent. */
typedef enum boxrig_ui_color_state {
    /** Cell belongs to the copy selection. */
    BOXRIG_UI_COLOR_SELECTED = 1u << 0,
    /** Cell belongs to the current search result. */
    BOXRIG_UI_COLOR_SEARCH_MATCH = 1u << 1,
    /** Cell belongs to the hovered hyperlink. */
    BOXRIG_UI_COLOR_HYPERLINK_HOVER = 1u << 2,
    /** Logically visible cursor occupies this cell, unless suppressed by IME. */
    BOXRIG_UI_COLOR_CURSOR_CELL = 1u << 3,
    /** Owning view has presentation focus. */
    BOXRIG_UI_COLOR_FOCUSED = 1u << 4
} boxrig_ui_color_state;

/** Straight, non-premultiplied eight-bit sRGB display colors. */
typedef struct boxrig_ui_color_filter_result {
    /** Glyph foreground for CELL/IME; reference foreground for SURFACE. */
    boxrig_rgb foreground;
    /** Background for CELL, IME, or SURFACE. */
    boxrig_rgb background;
    /** Underline for CELL/IME when not following the foreground. */
    boxrig_rgb underline;
    /** Decoration color for marker targets, including CURSOR. */
    boxrig_rgb marker;
    /** Resolve underline to the returned foreground after the callback. */
    bool underline_follows_foreground;
} boxrig_ui_color_filter_result;

/** Canonical input to a deterministic display-color filter. */
typedef struct boxrig_ui_color_filter_input {
    /** Display operation; unused color fields are zero. */
    boxrig_ui_color_target target;
    /** Original display colors, before this filter. */
    boxrig_ui_color_filter_result colors;
    /** Displayed foreground origin. */
    boxrig_ui_color_source foreground_source;
    /** Displayed background origin. */
    boxrig_ui_color_source background_source;
    /** Displayed underline origin. */
    boxrig_ui_color_source underline_source;
    /** Displayed marker origin. */
    boxrig_ui_color_source marker_source;
    /** Terminal rendition flags, retaining INVERSE after its application. */
    uint32_t attributes;
    /** Effective underline presentation after annotations. */
    boxrig_underline_style underline_style;
    /** Bitwise boxrig_ui_color_state values; states may overlap. */
    uint32_t state_flags;
    /** Effective cursor shape, otherwise normalized to BOXRIG_CURSOR_BLOCK. */
    boxrig_cursor_shape cursor_shape;
    /** Already filtered cell colors below a marker; marker field is unused. */
    boxrig_ui_color_filter_result underlay;
    /** Whether underlay names an actual cell below this marker. */
    bool underlay_valid;
    /** Background opacity, from 0 through 255; filters cannot change it.
     * CELL receives terminal background opacity. Markers receive the composed
     * cell and selection opacity, excluding images and pixels behind the view. */
    uint8_t background_opacity;
} boxrig_ui_color_filter_input;

/**
 * Filters resolved display colors without changing terminal state.
 * @param input Borrowed canonical input, valid only during this call.
 * @param context Application-owned context registered with the view.
 * @return Adjusted colors and underline inheritance policy.
 * @remarks Equal inputs and unchanged settings must produce equal results.
 * Invocation count and order are unspecified. Reentrant mutation, destruction,
 * or rendering of the same view, model, or renderer is forbidden. Neither
 * time nor unannounced external state may affect the result. Colors exclude
 * pixels behind the terminal surface, Kitty images and intrinsic color glyphs.
 */
typedef boxrig_ui_color_filter_result (*boxrig_ui_color_filter)(
    const boxrig_ui_color_filter_input *input,
    void *context);

/**
 * Builds an identity result suitable for selective overrides in a filter.
 * @param input Original filter input, or NULL for an all-zero result.
 * @return Original colors and underline inheritance policy.
 */
boxrig_ui_color_filter_result boxrig_ui_color_filter_result_default(
    const boxrig_ui_color_filter_input *input);

/**
 * Resolves a logical terminal color to straight eight-bit sRGB.
 * @param terminal Borrowed terminal owning palette and dynamic color state.
 * @param color Logical color to resolve.
 * @param default_role Dynamic role used when color.kind is DEFAULT.
 * @param rgb_out Receives the color only on success.
 * @return BOXRIG_STATUS_OK or BOXRIG_STATUS_INVALID_ARGUMENT.
 * @remarks This operation does not invoke a view filter or mutate the palette.
 */
boxrig_status boxrig_ui_color_resolve_rgb(
    const boxrig_terminal *terminal,
    boxrig_color color,
    boxrig_dynamic_color default_role,
    boxrig_rgb *rgb_out);

#ifdef __cplusplus
}
#endif

#endif
