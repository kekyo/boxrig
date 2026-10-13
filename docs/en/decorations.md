# Changing cursor and selection appearance

[日本語](../ja/decorations.md)

boxrig UI can display the cursor as a thin vertical bar and fill selections with color.
Shapes and colors are specified per view, allowing the same terminal to have different appearances.
These settings do not change character attributes held by boxrig core or the range being copied.

## Configure the cursor

This function changes the cursor to a vertical bar and disables its blinking.
Pass an existing UI view and continue normal frame processing to display the change.
See [UI step-by-step](./step-by-step/step-by-step-ui.md) for view creation and rendering.

```c
#include <boxrig/ui.h>

boxrig_ui_status use_steady_bar(boxrig_ui_view *view)
{
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BAR;
    style.blink = BOXRIG_UI_CURSOR_BLINK_STEADY;
    style.line_width_dip = 2.0f;
    return boxrig_ui_view_set_cursor_style(view, &style);
}
```

After normal rendering, a logically visible cursor becomes a 2 DIP vertical bar.
The line width is rounded to pixels according to DPI and output scale and clamped to the cell.
Terminal-requested cursor hiding, IME suppression, and focus conditions still apply.

## Follow terminal cursor settings

The default style follows the terminal's shape, color, and blinking settings.
Set shape to TERMINAL to use the block, underline, or bar requested by DECSCUSR.

The default block presentation is an outline.
Set `block_presentation` to FILL to fill the cell and draw its text in a readable color.
Explicit OUTLINE, UNDERLINE, and BAR shapes do not use that field.
Colors can follow the terminal cursor color or use fixed RGB.

`reset_cursor_style()` restores the view's defaults.
It does not reset terminal DECSCUSR or OSC 12 state.

## Change selection appearance

Use `boxrig_ui_selection_style` to select an outline, fill, or both and specify background, text, and outline colors and opacity.
The default is a blue outline.
`set_selection_style()` copies the settings, and `reset_selection_style()` restores defaults.

Fill alpha and outline alpha are independent.
You can also preserve the original text color or choose a fixed color.
These are display changes; the logical selection range and snapshot text are preserved.

## Combine with color filters and blinking

[Color filters](./color-filters.md) apply after decoration colors are chosen.
A FILL cursor uses the filtered colors and underlying cell to draw text.
Selection fill and outline each have their own filter roles as well.

[Blink settings](./blinking.md) control whether blinking is enabled and its intervals.
Even if the style requests blinking, the cursor stays steady when cursor blinking is disabled for the view.
Selection and cursor fills do not expose characters hidden by concealed attributes or text blinking.

## Configure each view separately

Each view holds its own style even when sharing a terminal and model.
For example, the interactive view can use a bar while another view follows terminal settings.
After changes, render according to normal frame requests.

See [boxrig_term](./boxrig_term.md) for demo controls.
The [public header](../../include/boxrig/ui/decoration.h) describes all style fields and value requirements.

[Documentation index](./index.md)
