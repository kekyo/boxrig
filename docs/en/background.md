# Configuring background colors, images, and transparency

[日本語](../ja/background.md)

boxrig UI can place colors and images behind a terminal and, on supported platforms, show the desktop through it.
For example, you can make only the background translucent while leaving text opaque.
You can also request a frosted-glass effect from the OS.

| Desired display | Setting to use |
| --- | --- |
| A solid color behind the terminal | View background color |
| A background image unaffected by scrolling | View background image |
| Windows or the desktop behind the terminal | Component transparency settings and parent-window preparation |
| A blurred desktop | Background effects on a supported OS |

## Set a background color

This function sets translucent red on an existing UI view.
Call it with terminal-background transparency configured, then advance normal frame processing to overlay red behind the text.
See [UI step-by-step](./step-by-step/step-by-step-ui.md) for view creation and [Transparent terminal backgrounds](#make-the-terminal-background-transparent) for transparency requirements.

```c
#include <boxrig/ui.h>

boxrig_ui_status set_red_background(boxrig_ui_view *view)
{
    return boxrig_ui_view_set_background_color(
        view, (boxrig_ui_rgba){255, 0, 0, 128});
}
```

This color is placed beneath the terminal.
Check the rendered result with terminal-background transparency enabled as well.
An alpha of 0 hides the added solid color.
This does not modify text colors or the terminal palette, so it is configured separately from [color filters](./color-filters.md).

## Background layer order

From front to back, the layers are terminal text and images, the added solid color, the background image, and underlying windows or the desktop.
They use source-over composition.
Opaque terminal-background areas hide colors and images behind them.

Specify colors as 8-bit sRGB without premultiplying alpha.
The terminal palette, added solid color, background image, and OS effects do not replace one another's settings.

## Supply a background image

Set RGBA8 pixels, width, height, stride, available byte count, overall opacity, and placement in `boxrig_ui_background_image`, then pass it to `set_background_image()`.
Pixels are copied on success, so you can release the source buffer afterward.
Failure preserves the previous image.

| Fit | Result |
| --- | --- |
| `COVER` | Fill the area while preserving aspect ratio, cropping excess around the center |
| `CONTAIN` | Center the entire image while preserving aspect ratio, leaving transparent margins |
| `STRETCH` | Scale width and height independently to the view area |

The constant prefix is `BOXRIG_UI_BACKGROUND_`.
Overall opacity and pixel alpha are multiplied exactly once.
Pass `NULL` to clear the image.
The image is fixed to the view; terminal scrolling and Kitty image deletion do not move or remove it.

The default retained-data limit is 64 MiB, the configurable maximum is 256 MiB, and each dimension is at most 16384 pixels.
Renderer capacity requirements must also be met.
The application decodes files; the current API has no tiling, additive blending, or multiplicative blending.

## Make the terminal background transparent

Use each component's transparency settings to specify background opacity and the requested scope.
Surface alpha and transparency from a toplevel to the desktop are separate capabilities.
You can also choose whether transparency is required or used only when available.

Check effective scope and opacity with `get_transparency()` or its equivalent instead of assuming success from requested values.
The parent's owner prepares the parent window.
For Win32 parent configuration in particular, see the [Win32 guide](./platforms/win32.md#transparency-and-parent-window-requirements).

<a id="すりガラス効果の要求"></a>

## Request an OS frosted-glass effect

The toplevel owner creates the platform-specific background effect provider.
If several terminals share one toplevel, the host aggregates their regions.
This setting does not blur terminal text or Kitty images themselves.

| Request | Result |
| --- | --- |
| `NONE` | Clear the host's request |
| `PREFER` | Request the effect when supported; otherwise continue normal transparency |
| `REQUIRE` | Return `UNAVAILABLE` if unsupported, or `BUSY` during Wayland discovery; preserve the previous request on failure |

`accepted` in `get_state()` means that the OS or protocol accepted the request.
It is not proof that actual pixels were blurred.
Wayland also requires a surface commit by the owner after configuration.

## Unsupported environments and distribution requirements

| Platform | Integration requirements |
| --- | --- |
| Win32 | [System backdrop](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type) on Windows 11 build 22621 or later |
| X11 | A running compositor advertising KDE-style blur support on the root window; uses the same `_KDE_NET_WM_BLUR_BEHIND_REGION` property as the [KDE implementation](https://github.com/KDE/kwindowsystem/blob/master/src/platforms/xcb/kwindoweffects.cpp) |
| Wayland | The blur capability of [ext-background-effect-v1](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/1.48/staging/ext-background-effect/ext-background-effect-v1.xml) |
| GTK 3 | An optional adapter connecting to the X11 or Wayland provider |
| GTK 4/Wayland | A usable route requires the [commit request API](https://docs.gtk.org/gdk4-wayland/method.WaylandSurface.force_next_commit.html) from 4.18 or later |
| GTK 4/X11 | Not currently provided; the standard integration was not adopted because the [XID retrieval API is deprecated](https://docs.gtk.org/gdk4-x11/method.X11Surface.get_xid.html) |

Optional SDK targets are `install-background-wayland`, `install-background-gtk3`, and `install-background-gtk4`.
These dependencies are not mandatory for normal GTK widgets.
Adapters built with GTK 4.14–4.16 report unavailable.

Native X11 regions use device pixels; Wayland and GTK adapters use logical coordinates.
A region count of 0 also differs: it means the whole area on native X11 and an empty region for GTK and Wayland.
Use providers on the UI thread and release them before the native resources they borrow.

[Background API](../../include/boxrig/ui/view.h), [Background effect state](../../include/boxrig/ui/background_effect.h), [Demo settings](./boxrig_term.md), [Documentation index](./index.md)
