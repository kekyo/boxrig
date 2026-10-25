# Using the bundled terminal

[日本語](../ja/boxrig_term.md)

`boxrig_term` is a terminal implementation example that uses the public boxrig core and boxrig UI APIs to interact with a shell.
Try colored text, images, selection and copying, IME, background transparency, and other features, then inspect the source to see how to integrate them into an application.
The screenshot below shows shell output in `boxrig_term`.

![Interacting with a shell in boxrig_term](../../images/shell.png)

This page explains how to start the example and try its display and controls.
The environment variables and shortcuts shown here are specific to `boxrig_term`.
Applications using the library configure each feature through its API.

## Purpose and preparation

In `boxrig_term`, core manages terminal contents and UI handles rendering and input; application code connects shell startup, communication, and the event loop.
See the [README examples](../../README.md#implementation-examples) for a short introduction to these roles, or [UI step-by-step](./step-by-step/step-by-step-ui.md) to build a terminal incrementally.
Follow the full implementation from the applications for [X11](../../examples/boxrig_term/x11/application.c), [GTK 3](../../examples/boxrig_term/gtk3/application.c), [GTK 4](../../examples/boxrig_term/gtk4/application.c), [Wayland](../../examples/boxrig_term/wayland/application.c), and [Win32](../../examples/boxrig_term/win32/application.c).

Follow the [build guide](./building.md), using `make native` for Linux and `make win32` for Windows.
Prepare the necessary GUI libraries and runtime environment.
Building core alone does not build the terminal demo.

To check consistent display contents, launch [boxrig_emitter](./boxrig_emitter.md) as the child process.
See [UI step-by-step](./step-by-step/step-by-step-ui.md) to learn library usage in order.

## Start on each platform

Run the command for your platform from the repository root.

```sh
./build/boxrig_term/x11/boxrig_term
./build/boxrig_term/gtk3/boxrig_term
./build/boxrig_term/gtk4/boxrig_term
./build/boxrig_term/wayland/boxrig_term
```

On Linux, the child process is `SHELL`, or `/bin/sh` if unset.
Place a specific program after `--`.

```sh
./build/boxrig_term/x11/boxrig_term -- /bin/sh -l
```

<a id="ネイティブwayland端末を実行する"></a>

Native Wayland requires a running Wayland compositor.
`BOXRIG_WAYLAND_DECORATION=auto` selects available server decorations, then libdecor, then no decorations.
`server` and `client` fail if the requested method is unavailable; `none` creates no decorations.
libdecor is an optional build dependency.

On Windows, run `build\boxrig_term\win32\amd64\boxrig_term.exe` or `i686\boxrig_term.exe` for the target ABI.
`boxrig_term_launcher.exe` must be in the same directory.
The default child is Command Prompt.
You can try it through Wine on Linux as follows, but record that separately from rendering and ConPTY verification on real Windows.

```sh
wine ./build/boxrig_term/win32/amd64/boxrig_term.exe
```

## Keyboard, mouse, copying, and links

| Action | Result |
| --- | --- |
| Left drag | Character selection |
| Double/triple click | Select a Unicode word / logical line joining wrapped rows |
| Ctrl+left drag | Rectangular row-and-column selection |
| Shift+drag | Select on the terminal side even while the child tracks the mouse |
| Ctrl+Shift+C | Copy selection to CLIPBOARD |
| Ctrl+Shift+V, Shift+Insert | Paste from CLIPBOARD |
| Middle click | Paste from PRIMARY where available on Linux |
| Right click | Copy a valid selection to CLIPBOARD when mouse tracking does not take precedence |
| Wheel | Normally scroll history by three rows; send input when child mouse tracking is enabled |
| Shift+wheel | Scroll terminal-side history |
| Ctrl+wheel | Increase/decrease text size |

Multiple clicks with Ctrl also select rectangular characters.
The destination chosen at selection start remains until completion, preventing duplicate PTY delivery even if modifiers change midway.
Rectangular copying separates display rows with LF and, by default, removes empty cells and ASCII spaces that continue to the visual right edge of the row.
If text exists to the right of the selection, spaces before it are retained.
Set `BOXRIG_BLOCK_COPY_TRIM_TRAILING_SPACES=0` to preserve spaces or `1` to trim them.
When this variable is set, Ctrl+Shift+F5 toggles the policy at runtime; copy the same selection again to observe the change.
Empty rows and their separating LF are retained. No extra final LF is added.
See [Selection and copying](./text.md#selection-and-copying) for full-width spaces, combining characters, and other details.
Linux also updates PRIMARY when selection ends.
Win32 does not change the system clipboard merely because selection ends.

The history scrollbar is disabled on the alternate screen.
Without mouse tracking, mode 1007 can turn the vertical wheel into Up/Down keys sent to the child.
See [Platform support](./reference/platform-support.md) for copy/paste limits and platform differences.

Open OSC 8 links and automatically recognized HTTP/HTTPS links with an unmodified left click.
The application waits after the click to distinguish it from dragging or multiple clicks.
Leaving the range, losing focus, or updating text cancels opening.
OSC 8 takes precedence; a disallowed URI is not replaced by an automatically recognized link.
URIs with empty hosts, whitespace/control characters, or more than 8,192 bytes are not opened.
`BOXRIG_AUTO_URLS=0` disables automatic recognition only.

## Change fonts and appearance

Ctrl+`+`/`-` increases/decreases text size; Ctrl+`0` restores the default.
The cursor defaults to a blinking block.
Run `printf '\033[5 q'` in the shell for a blinking bar, or `printf '\033[1 q'` for a block.
The space immediately before `q` is required.

| Environment variable | Feature to try | Toggle when enabled |
| --- | --- | --- |
| `BOXRIG_SCROLLBAR=always/auto/never` | Always show / show with history / hide the bar | Ctrl+Shift+F9 |
| `BOXRIG_SELECTION_STYLE_DEMO=1` | Selection decorations | Ctrl+Shift+F10 |
| `BOXRIG_CURSOR_STYLE_DEMO=1` | Cursor decorations | Ctrl+Shift+F11 |
| `BOXRIG_CURSOR_STYLE_DEMO=2` | Three focus-sensitive cursor boxes | Ctrl+Shift+F11 |
| `BOXRIG_COLOR_GLYPHS=0/1` | Monochrome/color glyphs such as emoji on Linux | Ctrl+Shift+F11 |
| `BOXRIG_COLOR_FILTER_DEMO=1` | Display color filters | Ctrl+Shift+F12 |
| `BOXRIG_AMBIGUOUS_WIDTH=narrow/wide/font` | One, two, or font-dependent columns for ambiguous characters | Set at startup |

When unset, the scrollbar is always shown and color fonts are enabled.
Ambiguous characters default to `font`: advances round up to whole columns and glyphs retain their natural width at the center. For example, a 2.1-cell advance occupies three columns. Set `narrow` or `wide` to use a fixed one-column or two-column width.
Demo toggle keys are active only when their corresponding environment variables are set.
Try the two F11 demos separately.

<a id="表示色の補正を試す"></a>

Color filtering cycles through an active filter, no filter, and an identity filter.
Terminal-stored colors and copied text do not change.

```sh
BOXRIG_COLOR_FILTER_DEMO=1 ./build/boxrig_term/x11/boxrig_term --renderer=x11
```

For API configuration, see [Fonts](./fonts-and-layout.md), [Color filters](./color-filters.md), [Decorations](./decorations.md), and [Scrollbars](./scrollbars.md).

## Choose input and IME

| Environment variable | Selection |
| --- | --- |
| `BOXRIG_GTK_IME=auto/gtk/ibus/fcitx5/fcitx4` | GTK automatic selection, normal GTK, direct IBus, or direct fcitx 5/4 |
| `BOXRIG_WAYLAND_IME=auto/wayland/ibus/fcitx5/fcitx4` | Native Wayland automatic selection, text-input-v3, direct IBus, or direct fcitx 5/4 |
| `BOXRIG_IME_CANDIDATES=auto/system/client` | Automatic, OS, or in-terminal candidates |

A nonempty `GTK_IM_MODULE` takes priority over GTK automatic selection.
Use `BOXRIG_GTK_IME=gtk` to select normal GTK only.
An explicit `wayland` selection on native Wayland also takes priority over in-terminal candidate settings.

The default AUTO policy uses configuration hints and running services to select a direct provider. An already selected fcitx GTK module is preserved.
Even when CLIENT is requested for fcitx 5/4, candidate ownership remains NONE until a valid panel notification arrives.

In-terminal candidates render candidate data from direct IBus or fcitx 5/4.
The normal GTK/Wayland routes do not expose the same candidate list.
The GTK 4 X11 route leaves candidate-coordinate handling to the GTK module, so selecting `ibus`, `fcitx5`, or `fcitx4` does not switch it to a direct connection.
Select candidates with the keyboard; clicks in their area are not sent to the PTY.
Candidates disappear on focus loss, and old candidates are not shown again when focus returns.
Check the effective route using `ime-route`, `direct-backend`, `direct-selection`, `candidate-owner`, and `candidate-reason` diagnostics.
Fcitx 4 in-terminal candidates combine preedit and candidates in a string panel. Individual candidate counts are unavailable, so `candidate-count=0` also applies while it is visible.
See [Input and IME](./input-ime.md#choose-an-input-route) for panel and compatibility-service limitations.

The normal demo does not provide an application-owned editable document.
An unsupported warning when IBus requests surrounding text therefore does not mean that the display cannot show text.
See [Input and IME](./input-ime.md) and [Platform support](./reference/platform-support.md) for editable documents and input-method limitations.

## Choose rendering and backgrounds

Place `--renderer=name` before the child-process separator `--`.

| Platform | Values | Selection with auto |
| --- | --- | --- |
| X11 | `auto`, `x11`, `opengl` | Try available ES/GL, then fall back to standard rendering |
| GTK 3/4 | `auto`, `cairo`, `opengl` | Prefer OpenGL, then fall back to Cairo |
| Native Wayland | `auto`, `wayland`, `opengl` | Use standard rendering if OpenGL creation returns UNAVAILABLE |
| Win32 | `auto`, `win32`, `opengl`, `d3d11` | Try ES/GL, then standard rendering; also depends on build and diagnostic settings |

Explicit `opengl` is mandatory and fails startup when unavailable.
Native Wayland does not switch automatically after a context failure during operation.
The GTK widget library itself defaults to Cairo, unlike this demo's auto setting.
With `BOXRIG_RENDERER_DEMO=1`, GTK also supports Ctrl+Shift+F6/F7/F8 for Cairo, OpenGL preferred, and OpenGL required.

This X11 example makes the terminal background transparent and places translucent red behind it.
It requires OpenGL and a window environment capable of composing transparency.

```sh
BOXRIG_BACKGROUND_OPACITY=0 BOXRIG_BACKGROUND_RGBA=255,0,0,128 \
    ./build/boxrig_term/x11/boxrig_term --renderer=opengl
```

Background opacity is 0–100%, and each RGBA component is 0–255.
`BOXRIG_BACKGROUND_IMAGE=cover/contain/stretch` tests placement of a built-in checkerboard; it does not take an image filename.

Ctrl+`<` decreases opacity by 10%; Ctrl+`>` increases it by 10%.
Ctrl+Shift+O switches between 100% and the previous translucent value, initially 80%.
`<` and `>` are logical symbols, so common keyboard layouts also require Shift.

Background effects use `BOXRIG_BACKGROUND_EFFECT=none/prefer/require`.
The default is none; prefer continues when unavailable, while require fails.
For optional GTK providers, use `gtk3/boxrig_term_effects` or `gtk4/boxrig_term_effects`, built with `make gtk-background-effects`.
See [Backgrounds](./background.md) for standard binaries and OS requirements.

On Win32, `BOXRIG_TRANSPARENCY_MODE=background/window/opaque` selects background-only, whole-window, or opaque rendering.
Initial whole-window opacity uses `BOXRIG_WINDOW_OPACITY=1..100`, defaulting to 100.
By default, unavailable background transparency falls back to whole-window opacity.
`BOXRIG_TRANSPARENCY_FALLBACK=opaque` disables that fallback.
At 0% background opacity, it returns to opaque instead of becoming completely transparent.

## Arguments and environment variables

| Setting | Value or purpose |
| --- | --- |
| `-- command ...` | Start the specified child process |
| `BOXRIG_TERM` | Defaults to `xterm-256color`; also accepts `xterm` |
| `BOXRIG_ALLOW_NOTIFICATIONS` | Allow notifications only when exactly `1` |
| `BOXRIG_ALLOW_GRAPHICS_EXTERNAL` | Allow external image data only when exactly `1` |
| `BOXRIG_AUTO_URLS` | `0` disables automatic URL recognition |
| `BOXRIG_CONPTY` | Win32: `system`, `packaged-preferred`, or `packaged-required` |
| `BOXRIG_CONPTY_DIRECTORY` | Absolute path to bundled ConPTY |

Linux notifications use a session D-Bus notification service; Windows uses Shell notifications.
See [Host integration](./host-integration.md) and [Platform support](./reference/platform-support.md) for rate limits and OSC 52 differences.

## Diagnostics and shutdown

Check startup diagnostics for the requested and actual backends.
The name `auto` alone does not tell you whether GPU rendering was selected.
See [Platform support](./reference/platform-support.md) for GPU, IME, and ConPTY conditions.

Closing the window ends communication and reaps the child process.
POSIX uses SIGHUP; Win32 runs shutdown processing including the launcher.
If Wine ConPTY treats Ctrl+C as shell termination, the application does not automatically create another shell that loses the previous state.
Shutdown behavior is covered by the [Win32 sample tests](../../tests/win32/test_win32_sample.c).

For VT sequence re-emission by ConPTY, see the [upstream discussion](https://github.com/microsoft/terminal/issues/1985).

[Documentation index](./index.md)
