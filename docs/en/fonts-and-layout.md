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

Ambiguous-width characters, such as `①`, can occupy either one or two terminal columns.
Choose the width with `boxrig_terminal_config.ambiguous_width` when creating the terminal.
The default is `BOXRIG_AMBIGUOUS_WIDTH_FONT`. Select `BOXRIG_AMBIGUOUS_WIDTH_NARROW` for a fixed one-column width or `BOXRIG_AMBIGUOUS_WIDTH_WIDE` for two columns.
Font-dependent mode derives columns from the actual font advance, the distance to the next character. Measurements include fallback fonts, bold, and italic styling.
Rendering, cursors, wrapping, and selection share this width. Match it to the applications running in the terminal.
See [Unicode East Asian Width](https://www.unicode.org/reports/tr11/tr11-44.html) and its [character data](https://raw.githubusercontent.com/unicode-org/unicodetools/main/unicodetools/data/ucd/17.0.0/EastAsianWidth.txt) for the classification.

With narrow or wide policy, glyphs fit the allocated columns. Changing the font does not change stored column counts.

Font-dependent policy divides the advance by the cell width and rounds up to an integer. A 2.1-cell advance occupies three columns, with the original glyph centered and 0.45 cells of padding on each side. Copying does not add padding spaces. This policy does not change the columns of ordinary Latin characters, wide characters, or emoji.

GTK, X11, Wayland, and Win32 terminal components connect font measurements before accepting input. Changing fonts or scale in a font-dependent terminal remeasures and reflows existing text, including history, while preserving the terminal connection.
For standalone X11, Win32, and OpenGL renderers, call `boxrig_*_renderer_bind_view()` before input and after font changes. Preparing a frame through the common renderer lifecycle also connects measurements. Core-only applications can register a synchronous callback with `boxrig_terminal_set_font_width_resolver()`; return the advance divided by the cell width.

Without a resolver, or when measurement is unavailable or invalid, new characters use one column. Detaching a resolver preserves existing text and widths. A character can occupy up to 255 columns; larger measurements return `LIMIT_EXCEEDED`. Characters wider than the entire viewport temporarily occupy one column and are remeasured when a wider viewport can accommodate them.
Font measurements do not reveal the column widths expected by remote applications; select narrow or wide when needed.

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
