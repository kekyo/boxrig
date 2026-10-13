# Applying color filters per view

[日本語](../ja/color-filters.md)

boxrig UI color filters can display the same terminal in normal colors and high contrast simultaneously.
They change each view's drawing colors without changing the text or colors held by boxrig core.

## What color filters can change

You can adjust colors according to the drawing role: cells, backgrounds, cursors, selection and search outlines, hovered links, IME, and others.
Text, attributes, and copied contents remain unchanged.
Background alpha and the pixels of Kitty images and color glyphs themselves are outside the filter's scope.

## Register a filter

This example changes cell and IME text to white.
`white_text()` selects the colors; `use_white_text()` registers it on an existing UI view.
Continue normal application frame processing after registration.

```c
#include <boxrig/ui.h>

static boxrig_ui_color_filter_result white_text(
    const boxrig_ui_color_filter_input *input, void *context)
{
    (void)context;
    /* Keep the original colors for targets that are not changed below. */
    boxrig_ui_color_filter_result colors =
        boxrig_ui_color_filter_result_default(input);

    if (input->target == BOXRIG_UI_COLOR_CELL ||
        input->target == BOXRIG_UI_COLOR_IME)
        colors.foreground = (boxrig_rgb){255, 255, 255};
    return colors;
}

boxrig_ui_status use_white_text(boxrig_ui_view *view)
{
    return boxrig_ui_view_set_color_filter(view, white_text, NULL);
}
```

Normal frame processing then changes the selected foreground colors to white.
First obtain the original colors from the default function, then override only the fields you want to change.
Filtering does not reveal text with the concealed attribute.

## Text, background, and cursor colors

Inputs include the resolved original colors, color origins, attributes, selection state, and focus state.
RGB is non-premultiplied sRGB.
Effects such as inverse have already been applied; do not apply the same inversion again in the filter.

If the underline follows the foreground, it resolves to the returned foreground color.
Cursor and outline filters can also inspect the filtered colors of the underlying cell.
Blink phase is not passed to the callback, and a phase-only change reuses filtered colors.

## Update settings and release resources

Return the same result for the same inputs and settings.
Call counts and order are not guaranteed.
The callback is unsuitable for results that depend on unannounced external state or the current time.

The application owns the context.
After changing its contents, request recalculation with `boxrig_ui_view_invalidate_colors()`.
Remove the filter with `set_color_filter(view, NULL, NULL)`.
Keep the context alive until removal or view destruction finishes.
Do not update, render, or destroy the same view or model inside the callback.

## Custom rendering and limits

In a custom renderer, resolve the original display colors with `boxrig_ui_color_resolve_cell()`, supply states such as selection, and pass them to `boxrig_ui_view_resolve_colors()`.
Retain the result with its corresponding drawing operation, and do not feed it back as the next filter input.

The [runnable example](../../examples/ui/color_filter.c) chooses black or white based on background brightness and demonstrates updating and removing settings.
It also checks that colors stored in core cells remain unchanged.
Treat retention beyond the configured limit and reentry during filtering as failures.

[Color filter API](../../include/boxrig/ui/color.h), [View API](../../include/boxrig/ui/view.h), [Documentation index](./index.md)
