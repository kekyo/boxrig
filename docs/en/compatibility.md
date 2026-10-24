# Compatibility

[日本語](../ja/compatibility.md)

This overview explains the scope and required configurations for the [features in the README](../../README.md#features).
boxrig core interprets text and image control sequences, and boxrig UI integrates display and input.
Start with the feature overview, then check the requirements for your platform.
The references at the end contain control-sequence details, minimum OS requirements, and verified environments.

## Reading the support tables

“Supported” refers to functionality handled by the public API or bundled implementations.
Availability depends on the OS, rendering backend, input method, and application integration.
Minimum requirements in code are not the same as environments verified by running the software.

## Terminal feature overview

Text retrieval, search, and input-sequence generation work with core alone.
Font rendering, preedit and candidates, and selection decorations use UI.
Your application connects shell communication and OS operations such as titles and notifications.

| Purpose | Supported functionality | Details |
| --- | --- | --- |
| Text and controls | UTF-8, combining characters, emoji, ANSI and RGB colors, decorations, blinking, screen, and history | [Control sequences](./reference/terminal-sequences.md) |
| Input | Newline settings, Shift_JIS conversion, key mappings, Kitty keyboard, paste, mouse, and focus | [Terminal I/O settings](./terminal-io.md) |
| Working with text | Snapshots, selection, exact search, occurrence monitoring, and bounded regular-expression recognition | [Text](./text.md) |
| Images | Kitty graphics still images, placement, external transfers, and animation | [Graphics specification](./reference/graphics.md) |
| Host integration | Requests for titles, links, clipboard access, notifications, and external images | [Host integration](./host-integration.md) |
| Appearance | Fonts, zoom, backgrounds, transparency, color filters, cursor, and selection | [Documentation index](./index.md) |

## Platform differences

UI integrates with X11, GTK 3, GTK 4, native Wayland, and Win32.
In addition to standard rendering, optional OpenGL and Windows Direct3D 11 are available.
Core alone needs no window or rendering library.

Background effects, candidate windows, the clipboard, and accessibility also depend on OS facilities.
Check requirements in the [platform support tables](./reference/platform-support.md).

## IME surrounding text and synchronous editing

Preedit display and sending ordinary IME commits are supported.
Registering an application-owned editable document also enables surrounding-text access and synchronous editing.

A connection that only supplies surrounding text differs from an owner connection that can commit edit requests.
On GTK 3/4, native Wayland, and XIM, connecting an edit owner applies document deletion and commits exactly once.
Win32 handles surrounding documents and reconversion, but ordinary IME commits continue to go to the PTY.
A common configuration function does not mean that every platform has the same editing route.

See [Input and IME](./input-ime.md) for integration and [Platform support](./reference/platform-support.md#ime-notifications-clipboard-and-accessibility) for input-method-specific requirements.

## Minimum Windows requirements by layer

Core and basic communication, text rendering, GPU rendering, and ConPTY require different Windows versions.
Being able to link core for an older OS does not imply that the entire terminal demo works there.

The SDK's base layer targets Windows 2000 for i686 and Windows XP x64 for amd64.
The standard UI requires DirectWrite, D3D11 requires capabilities including Direct2D 1.1, and OS-supplied ConPTY requires Windows 10 1809 or later.
See [Windows requirements by layer](./reference/platform-support.md#windows-requirements-by-layer) for detailed versions and verification scope.

<a id="osc-99-desktop-notification"></a>

## Notification support

OSC 99 titles, bodies, identifiers, display conditions, and urgency are parsed.
The bundled terminal connects to notification providers on all five platforms but rejects display by default.
It permits notifications only when started with `BOXRIG_ALLOW_NOTIFICATIONS=1`, and also limits their frequency.

A query about notification parsing capabilities does not report OS display permission.
See [Control sequences](./reference/terminal-sequences.md#osc-99-notifications) and [Platform support](./reference/platform-support.md) for details.

## Unsupported features

sixel and iTerm2 images and printer output are unsupported.
In addition to the terminal role, GTK 3/4 provides terminal-text publication, disabled by default. When enabled, assistive technologies can retrieve visible text, the caret, selections, character attributes, and change notifications.
GTK 4 does not provide text-coordinate retrieval or assistive-technology requests to change the caret or selection. See [GTK](./platforms/gtk.md#retrieve-text-through-assistive-technologies) for setup and scope.

Regular-expression recognition has syntax and work limits, and exact search has no Unicode case-folding or normalization options.
The earlier [search transformation prototype](./search-capability-prototype.md) is not a public feature.

## Detailed references

| What to look up | Document |
| --- | --- |
| Accepted controls, responses, and outgoing keys | [Control sequences](./reference/terminal-sequences.md) |
| Image formats, placements, transfers, and limits | [Graphics specification](./reference/graphics.md) |
| OS, IME, and rendering requirements and verification scope | [Platform support](./reference/platform-support.md) |
| Public headers for your task | [API index](./reference/api.md) |
| Bundled files, runtime dependencies, and licenses | [Distribution](./reference/distribution.md) |

[Documentation index](./index.md)
