# boxrig documentation

[日本語](../ja/index.md)

boxrig is a C99 library for embedding terminal functionality in applications.
The [README](../../README.md) introduces its features and screenshots, followed by short UI and core examples.
Use this index to find feature guides, platform integration instructions, and detailed support information.

## Getting started

Use boxrig UI to embed a terminal in an existing window, or boxrig core for custom rendering and processing without a display.
Try the examples in [Getting started](./getting-started.md), then build a small application with a step-by-step guide.

| Document | What it covers |
| --- | --- |
| [README](../../README.md) | What boxrig can do, with short code examples |
| [Getting started](./getting-started.md) | Choosing a configuration and running examples |
| [Building and installing](./building.md) | Sources, dependencies, SDKs, and linking |
| [Basic concepts](./architecture.md) | The roles of core, UI, communication, and your application |

## Adding features

Text retrieval, search, and terminal I/O settings are available in boxrig core without a display.
boxrig UI handles fonts, backgrounds, IME, and selection display.
Each guide presents the feature and a code example before explaining its settings and conditions.

| Document | What it covers |
| --- | --- |
| [Terminal I/O settings](./terminal-io.md) | Newlines, key sequences, and character encodings |
| [Retrieving, selecting, and searching text](./text.md) | Text extraction, exact search, monitoring, and recognition |
| [Regular expressions for arbitrary text](./regex.md) | UTF-8 matching, diagnostics, and resumable execution without a terminal |
| [Input and IME](./input-ime.md) | Preedit, candidates, and editing application-owned documents |
| [Fonts and layout](./fonts-and-layout.md) | Fonts, text size, DPI, and grid dimensions |
| [Host integration](./host-integration.md) | Titles, the clipboard, notifications, and external images |
| [Backgrounds](./background.md) | Background colors, images, transparency, and OS effects |
| [Display color filters](./color-filters.md) | Changing colors for each view |
| [Cursor and selection](./decorations.md) | Shapes and appearance |
| [Blinking](./blinking.md) | Text and cursor blinking |
| [Scrollbars](./scrollbars.md) | Built-in and application-provided scrollbars |

## Integrating into an application

The [UI step-by-step guide](./step-by-step/step-by-step-ui.md) builds a terminal starting with fixed text.
Use the [core step-by-step guide](./step-by-step/step-by-step-core.md) if you also want to implement rendering yourself.

[boxrig_term](./boxrig_term.md) is a terminal implementation example that combines display, input, and shell communication on each platform.
You can try its display and controls, then inspect the source to see how to connect these operations to an application.

| Document | What it covers |
| --- | --- |
| [Communication sessions](./sessions.md) | Sending, receiving, waiting, and shutdown with PTYs or custom transports |
| [X11](./platforms/x11.md) | Connecting a parent window and event loop |
| [GTK 3/4](./platforms/gtk.md) | Widget allocation and frame processing |
| [Native Wayland](./platforms/wayland.md) | Child surfaces, event waiting, and parent commits |
| [Win32](./platforms/win32.md) | Child windows, messages, and DPI |
| [Rendering options](./renderers.md) | Default rendering, OpenGL, and custom targets |
| [Direct3D 11 component](./d3d11-component.md) | A GPU-rendered Win32 child window |
| [Direct3D 11 renderer](./d3d11-renderer.md) | Rendering to an application-owned texture |

## Looking up specifications

Start with [Compatibility](./compatibility.md) to check control sequences and platform support.
[boxrig_emitter](./boxrig_emitter.md) emits control sequences for testing.

| Document | What it covers |
| --- | --- |
| [Control sequences](./reference/terminal-sequences.md) | Accepted sequences, queries, and input protocols |
| [Graphics specification](./reference/graphics.md) | Kitty graphics transfers, placements, limits, and responses |
| [Platform support](./reference/platform-support.md) | OS, rendering, IME, and notification requirements and verification scope |
| [API index](./reference/api.md) | Public headers organized by task |
| [Distribution](./reference/distribution.md) | SDK contents, runtime dependencies, bundled files, and licenses |
| [SDK README](../../README_pack.md) | Running your first example from an SDK |

## Developing and testing boxrig

| Document | What it covers |
| --- | --- |
| [Testing](./development/testing.md) | Full, feature, SDK, GUI, and physical-machine tests |
| [Internal design](./development/internals.md) | Modules, state, output ordering, and rendering resources |
| [Text workload measurements](./text-workload-measurements.md) | Work and retained data for search, monitoring, recognition, and copying |
| [Search transformation prototype](./search-capability-prototype.md) | Unicode transformation decisions and differences from the public API |
