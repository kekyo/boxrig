# Configuring fonts and layout

[日本語](../ja/fonts-and-layout.md)

boxrig UI lets you change terminal fonts and text size and specify fallback and color fonts.
Larger text also changes the number of rows and columns that fit in a window, so retrieve actual cell dimensions and apply them to layout.
boxrig core holds text and column widths; UI selects fonts and places text on screen.

## Specify a font and text size

This function changes an existing X11 component to a 16 DIP monospace font.
Continue component allocation and frame processing to display text at the new size.
The common `boxrig_font_specification` specifies family, size, weight, style, line spacing, fallback fonts, and related settings.

```c
#include <boxrig/x11.h>

boxrig_ui_status set_terminal_font(boxrig_x11_terminal_component *component)
{
    const boxrig_font_family family = {
        .name = "monospace", .name_length = 9,
        .resource_id = BOXRIG_FONT_RESOURCE_NONE
    };
    boxrig_font_specification font = boxrig_font_specification_default();
    font.families = &family;
    font.family_count = 1;
    font.size_dip = 16.0f;
    return boxrig_x11_terminal_component_set_font_specification(component, &font);
}
```

A DIP is based on 1/96 inch.
Settings are copied on success; failure preserves the previous settings and effective metrics.
GTK, Wayland, and Win32 have corresponding setters.
On-screen text changes after normal allocation and frame processing complete.

## Unicode and fallback fonts

Core retains Unicode characters and cell widths, and the UI text system maps them to available fonts.
Even with Shift_JIS input, text passed to fonts is Unicode.

Fallback candidates can be restricted to the whole system, registered resources, or specified families.
File and memory font registration APIs let you supply fonts bundled with your application.
Check each registration API for file lifetime and memory-copy requirements.
Redistribution of a font also follows that font's license.

## Complex text and color fonts

Text layout uses Pango on Linux and DirectWrite on Windows.
For combining characters, bidirectional text, and ligatures, stored logical positions may differ from screen positions.
Copying and selection use mappings supplied by text layout.

You can specify OpenType features and variable-font axes, but availability depends on the rendering system and font.
Set color-glyph permission with `boxrig_font_rendering_policy.color_glyphs`.
Allowing color glyphs does not make every color-font format available.
Check actual capabilities and missing-character diagnostics.

## Grid dimensions and display size

Confirm terminal grid dimensions separately from window pixel dimensions.
Retrieve cell metrics with the component's `get_effective_metrics()` and pass the actual allocated area to `allocate()`.
Resize core and the PTY according to the returned layout and requests.

If a font change fails, continue displaying with the old effective metrics.
Do not apply dimensions guessed from the requested font size as if the change had succeeded.

## DPI, scale, and zoom

DPI represents output resolution; scale is additional magnification applied by the UI or compositor.
Common rendering settings keep these separate.
On Wayland, allocation uses surface logical coordinates while the render target uses physical pixels.

Component visual zoom advances together with changes to input positions, IME, and layout.
Parent size requests or a new frame may still be pending after a requested change is accepted.
Continue normal frame processing and complete size requests.

## Multiple views and resizing

You can attach multiple views to the same model.
However, the terminal grid and shared text metrics must remain consistent, and only one view determines layout.
A display-only attachment cannot establish conflicting dimensions.

If settings conflict, inspect results such as `UNAVAILABLE` and either use separate models or choose a configuration with the same effective metrics.
See [Basic concepts](./architecture.md) and the [platform guides](./index.md#integrating-into-an-application) for parent, model, and view responsibilities.

[Font and text layout API](../../include/boxrig/ui/text_layout.h), [Documentation index](./index.md)
