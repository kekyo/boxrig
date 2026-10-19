/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_DECORATION_H
#define BOXRIG_UI_DECORATION_H

#include "boxrig/terminal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How a view selects the insertion-cursor shape. */
typedef enum boxrig_ui_cursor_shape_policy {
    /** Follow the terminal's DECSCUSR shape. */
    BOXRIG_UI_CURSOR_SHAPE_TERMINAL = 0,
    /** Use a full cell, outlined when presentation is DEFAULT. */
    BOXRIG_UI_CURSOR_SHAPE_OUTLINE,
    /** Draw a horizontal stroke near the bottom of the cursor cell. */
    BOXRIG_UI_CURSOR_SHAPE_UNDERLINE,
    /** Draw a vertical stroke near the leading edge of the cursor cell. */
    BOXRIG_UI_CURSOR_SHAPE_BAR,
    /** Use a full cell box, independent of the terminal's DECSCUSR shape. */
    BOXRIG_UI_CURSOR_SHAPE_BLOCK
} boxrig_ui_cursor_shape_policy;

/** How a view chooses the cursor's color before display-color filtering. */
typedef enum boxrig_ui_cursor_color_policy {
    /** Follow the terminal's dynamic cursor color, including OSC 12/112. */
    BOXRIG_UI_CURSOR_COLOR_TERMINAL = 0,
    /** Use the style's fixed RGB value with a UI color source. */
    BOXRIG_UI_CURSOR_COLOR_RGB
} boxrig_ui_cursor_color_policy;

/** View-level cursor blinking; intervals remain in boxrig_ui_blink_config. */
typedef enum boxrig_ui_cursor_blink_policy {
    /** Follow the terminal's requested blinking state. */
    BOXRIG_UI_CURSOR_BLINK_TERMINAL = 0,
    /** Keep the cursor steadily visible when logically visible. */
    BOXRIG_UI_CURSOR_BLINK_STEADY,
    /** Request blinking even when the terminal requests a steady cursor. */
    BOXRIG_UI_CURSOR_BLINK_ENABLED
} boxrig_ui_cursor_blink_policy;

/** Presentation of a terminal-selected block cursor. */
typedef enum boxrig_ui_cursor_block_presentation {
    /** Preserve the traditional cell outline. */
    BOXRIG_UI_CURSOR_BLOCK_OUTLINE = 0,
    /** Fill the cell and redraw its visible glyph with contrasting ink. */
    BOXRIG_UI_CURSOR_BLOCK_FILL
} boxrig_ui_cursor_block_presentation;

/** How the selected cursor box is painted, independent of its shape. */
typedef enum boxrig_ui_cursor_presentation {
    /** Preserve shape-specific strokes and the block_presentation setting. */
    BOXRIG_UI_CURSOR_PRESENTATION_DEFAULT = 0,
    /** Draw the perimeter of the selected box. */
    BOXRIG_UI_CURSOR_PRESENTATION_OUTLINE,
    /** Fill the selected box. A full cell redraws its visible glyph. */
    BOXRIG_UI_CURSOR_PRESENTATION_FILL,
    /** Fill while focused; draw the same box's perimeter while unfocused. */
    BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED
} boxrig_ui_cursor_presentation;

/** Independent insertion-cursor presentation owned by one view. */
typedef struct boxrig_ui_cursor_style {
    /** Shape policy, independent from color and blinking. */
    boxrig_ui_cursor_shape_policy shape;
    /** Source of the base cursor color. */
    boxrig_ui_cursor_color_policy color_policy;
    /** Fixed color used only for BOXRIG_UI_CURSOR_COLOR_RGB. */
    boxrig_rgb color;
    /** Blinking policy; disabled cursor blinking still means steady display. */
    boxrig_ui_cursor_blink_policy blink;
    /**
     * Positive finite stroke width in device-independent pixels.
     * Renderers multiply by output DPI / 96 and output scale, round to the
     * nearest device pixel, and clamp from one pixel through the cell size.
     */
    float line_width_dip;
    /**
     * Presentation when shape follows a terminal block cursor. Explicit
     * OUTLINE, UNDERLINE and BAR policies ignore this field. Explicit BLOCK
     * uses this field too, unless presentation is non-DEFAULT. FILL uses the
     * filtered cursor color and the effective cell background as glyph ink,
     * falling back to black or white when their brightness is too similar.
     * Concealment, text blinking and the one-visual-cell clip remain in force.
     */
    boxrig_ui_cursor_block_presentation block_presentation;
    /**
     * Painting policy for every shape. Non-DEFAULT values supersede
     * block_presentation and use box_width_dip for BAR and UNDERLINE.
     * FOCUSED uses boxrig_ui_view_set_focused(); an unfocused cursor is steady.
     * OUTLINE shape always selects a full cell; this policy chooses its paint.
     */
    boxrig_ui_cursor_presentation presentation;
    /**
     * Positive finite BAR width / UNDERLINE height in DIP, default four.
     * Scales and clamps like line_width_dip; independent of the outline width.
     * Non-DEFAULT boxes touch the leading or bottom cell edge. If the cell is
     * too small to leave an interior pixel, its outline may appear filled.
     */
    float box_width_dip;
} boxrig_ui_cursor_style;

/**
 * Creates terminal-following cursor decoration with a one-DIP block outline.
 * @return A copied style following the terminal's shape, color and blinking,
 * with block_presentation set to OUTLINE, presentation to DEFAULT, and
 * box_width_dip to four.
 * @remarks Logical cursor hiding, IME suppression, and presentation focus
 * retain precedence over every style. This never changes terminal state.
 */
boxrig_ui_cursor_style boxrig_ui_cursor_style_default(void);

/** Visible decoration of the existing logical or rectangular copy selection. */
typedef enum boxrig_ui_selection_presentation {
    /** Draw only the perimeter of each selected visual range. */
    BOXRIG_UI_SELECTION_OUTLINE = 0,
    /** Paint a background beneath the selected text, without a perimeter. */
    BOXRIG_UI_SELECTION_FILL,
    /** Paint both the background and the perimeter. */
    BOXRIG_UI_SELECTION_FILL_OUTLINE
} boxrig_ui_selection_presentation;

/** How selection decoration chooses the glyph foreground before filtering. */
typedef enum boxrig_ui_selection_foreground_policy {
    /** Preserve the terminal and annotation foreground. */
    BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL = 0,
    /** Use the style's fixed foreground with a UI color source. */
    BOXRIG_UI_SELECTION_FOREGROUND_RGB
} boxrig_ui_selection_foreground_policy;

/** Copied view-owned selection decoration, independent of the copied text. */
typedef struct boxrig_ui_selection_style {
    /** Perimeter, background, or both. */
    boxrig_ui_selection_presentation presentation;
    /** Perimeter RGB before SELECTION_MARKER color filtering. */
    boxrig_rgb marker;
    /** Selection background RGB before CELL color filtering. */
    boxrig_rgb background;
    /** Whether selected glyphs keep their original foreground. */
    boxrig_ui_selection_foreground_policy foreground_policy;
    /** Selected glyph RGB used only with FOREGROUND_RGB. */
    boxrig_rgb foreground;
    /** Background opacity, from transparent 0 through opaque 255. */
    uint8_t background_opacity;
    /** Perimeter opacity, independent of background and glyph opacity. */
    uint8_t marker_opacity;
    /** Positive finite DIP width, rounded and clamped like cursor strokes. */
    float line_width_dip;
} boxrig_ui_selection_style;

/**
 * Creates the traditional opaque blue one-DIP selection outline.
 * @return A copied style preserving the original selected glyph colors.
 * @remarks Background fill, when selected, defaults to opaque blue. Selection
 * colors never reveal concealed glyphs or change snapshots and clipboard data.
 */
boxrig_ui_selection_style boxrig_ui_selection_style_default(void);

#ifdef __cplusplus
}
#endif

#endif
