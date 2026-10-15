# Public API index

[日本語](../../ja/reference/api.md)

Find public headers by task, such as interpreting terminal output, searching text, or embedding a terminal in a window.
Use boxrig core for processing without a display or for custom rendering, and boxrig UI for platform-specific rendering and input handling.
The public API is C99 and can also be called from C++.

See the [README examples](../../../README.md#implementation-examples) for minimal code and results, and [Basic concepts](../architecture.md) for object relationships.
The following index is organized by purpose.
Header comments at each link specify the individual API contracts.

## Core and communication

| Purpose | Public header | Guide |
| --- | --- | --- |
| Terminal creation, parsing, cells, history, output, and host requests | [terminal.h](../../../include/boxrig/terminal.h) | [Core step-by-step](../step-by-step/step-by-step-core.md) |
| Text, selection, search, monitoring, recognition, images, and time | [terminal.h](../../../include/boxrig/terminal.h) | [Text](../text.md), [Graphics specification](./graphics.md) |
| Regular expressions, captures, and name validation before matching | [regex.h](../../../include/boxrig/regex.h) | [Regular expressions (Japanese)](../../ja/regex.md) |
| Nonblocking communication sessions | [session.h](../../../include/boxrig/session.h) | [Sessions](../sessions.md) |
| POSIX file descriptors and PTYs | [posix.h](../../../include/boxrig/posix.h) | [Sessions](../sessions.md) |
| Win32 communication and PTYs | [session.h](../../../include/boxrig/win32/session.h), [pty.h](../../../include/boxrig/win32/pty.h) | [Win32](../platforms/win32.md) |

## UI and input

| Purpose | Public header | Guide |
| --- | --- | --- |
| Models and change synchronization | [ui.h](../../../include/boxrig/ui.h) | [Basic concepts](../architecture.md) |
| Views, allocation, frames, and common components | [view.h](../../../include/boxrig/ui/view.h), [component.h](../../../include/boxrig/ui/component.h) | [UI step-by-step](../step-by-step/step-by-step-ui.md) |
| Keys, shortcuts, and pasting | [input.h](../../../include/boxrig/ui/input.h), [shortcut.h](../../../include/boxrig/ui/shortcut.h), [input_text.h](../../../include/boxrig/ui/input_text.h) | [Terminal I/O settings](../terminal-io.md) |
| IME preedit, candidates, and editable documents | [composition.h](../../../include/boxrig/ui/composition.h), [candidates.h](../../../include/boxrig/ui/candidates.h), [editable.h](../../../include/boxrig/ui/editable.h) | [Input and IME](../input-ime.md) |
| Direct IME protocol, connection state, and selection diagnostics | [ime.h](../../../include/boxrig/ime.h) | [Input route selection](../input-ime.md#choose-an-input-route) |
| Fonts and text layout | [text_layout.h](../../../include/boxrig/ui/text_layout.h) | [Fonts and layout](../fonts-and-layout.md) |
| Time, blinking, and waiting | [animation.h](../../../include/boxrig/ui/animation.h), [blink.h](../../../include/boxrig/ui/blink.h), [animation_poll.h](../../../include/boxrig/ui/animation_poll.h) | [Blinking](../blinking.md) |
| Colors, background images, cursors, selection, and scrollbars | [color.h](../../../include/boxrig/ui/color.h), [image.h](../../../include/boxrig/ui/image.h), [decoration.h](../../../include/boxrig/ui/decoration.h), [scrollbar.h](../../../include/boxrig/ui/scrollbar.h) | [Backgrounds](../background.md), [Decorations](../decorations.md), [Scrollbars](../scrollbars.md) |

## Platform components

| Platform | Public headers and guide |
| --- | --- |
| X11 | [x11.h](../../../include/boxrig/x11.h), [component.h](../../../include/boxrig/x11/component.h), [Integration guide](../platforms/x11.md) |
| GTK 3 | [gtk3.h](../../../include/boxrig/gtk3.h), [terminal_widget.h](../../../include/boxrig/gtk3/terminal_widget.h), [Integration guide](../platforms/gtk.md) |
| GTK 4 | [gtk4.h](../../../include/boxrig/gtk4.h), [terminal_widget.h](../../../include/boxrig/gtk4/terminal_widget.h), [Integration guide](../platforms/gtk.md) |
| Native Wayland | [wayland.h](../../../include/boxrig/wayland.h), [Integration guide](../platforms/wayland.md) |
| Win32 | [win32.h](../../../include/boxrig/win32.h), [component.h](../../../include/boxrig/win32/component.h), [Integration guide](../platforms/win32.md) |

The same directories contain input adapters, host requests, and transparency details for each platform.
A setting in the common interface does not imply that every platform implements the same feature.
Check [Platform support](./platform-support.md).

## Additional rendering, notification, and background modules

OpenGL uses [opengl.h](../../../include/boxrig/render/opengl.h) and [runtime.h](../../../include/boxrig/render/runtime.h); Direct3D 11 uses [d3d11.h](../../../include/boxrig/render/d3d11.h).
See [Rendering options](../renderers.md) for the difference between platform child windows and renderers that draw to borrowed targets.

Connect notifications with [notification.h](../../../include/boxrig/ui/notification.h), [notification_dispatch.h](../../../include/boxrig/ui/notification_dispatch.h), and [freedesktop.h](../../../include/boxrig/notification/freedesktop.h).
Background effects use [background_effect.h](../../../include/boxrig/ui/background_effect.h) and platform providers.
See [Host integration](../host-integration.md) and [Backgrounds](../background.md) for usage.

## Reading APIs and result codes

Public definitions are in `include/boxrig/`.
Header comments describe arguments, return values, borrowing periods, and state after failure.
Applications do not need to include internal headers.

Handle core `boxrig_status` values as follows.
Platform APIs also have dedicated result types and diagnostics; do not convert them by assuming identical numeric values.

| Status | Caller action |
| --- | --- |
| OK | Processing completed |
| INVALID_ARGUMENT | Check arguments and setting combinations |
| OUT_OF_MEMORY, LIMIT_EXCEEDED | Check memory availability and configured limits |
| WOULD_BLOCK | Advance queues or related work, then retry the unprocessed portion according to the contract |
| INCOMPLETE | Current recognition results are incomplete; advance scanning or rescanning before retrieving them |
| STALE | Retained positions or ranges are outdated; retrieve them again |
| BUSY | Another owner or unfinished operation exists |

For `feed()`, check both status and consumed byte count.
For an outgoing event returning `WOULD_BLOCK`, retry the whole event.
[Terminal I/O settings](../terminal-io.md) and [Sessions](../sessions.md) illustrate this distinction.

## Ownership and related guides

Destroy borrowing objects before the terminals, models, and views they reference.
The usual order is view, model, then terminal.
Keep external displays, parent windows, GL contexts, and D3D devices alive until their respective borrowing contracts end.

Pointers returned for cells or host requests may not remain valid after the next mutation.
Use snapshots for long-lived text, and observe completion/cancellation and destruction ordering for asynchronous requests.
[Basic concepts](../architecture.md), [Text](../text.md), and [Host integration](../host-integration.md) describe specific lifetimes.

[Documentation index](../index.md)
