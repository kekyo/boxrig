# Investigating internal design

[日本語](../../ja/development/internals.md)

This document helps developers changing boxrig locate the relevant modules and dependencies.
It explains the boundaries that keep core independent, terminal-state and rendering-resource management, and platform integration points.
For roles and ownership relevant to applications using boxrig, see [Basic concepts](../architecture.md); for public APIs, see the [API index](../reference/api.md).

## Modules and dependency direction

Core handles terminal state and control sequences and does not own OS windows, PTYs, or fonts.
Common UI uses the public core API; platform input and renderers connect to common UI.
Sessions connect core to nonblocking transports.

```text
application
  ├─ X11 / GTK / Wayland / Win32 ─ UI ─ core
  └─ POSIX / Win32 transport ─ session ─ core
```

utf8proc types and functions remain private to core.
Common UI and platform implementations compile without `src/core` or utf8proc include paths.
Tests that build a custom UI using only installed public artifacts, together with undefined-symbol checks on archives, verify this boundary.

utf8proc for Unicode processing and libpng and zlib for PNG images are bundled into the core static library.
Core independence means handling terminal state without connecting to OS windows or I/O devices.
See the [Makefile](../../../Makefile) for dependency-source integration and [Distribution](../reference/distribution.md) for deployment requirements.

## Parsing and terminal state

[parser.c](../../../src/core/parser.c) implements the byte-stream state machine; [screen.c](../../../src/core/screen.c) handles screens, cursors, attributes, margins, and editing.
Retaining partial UTF-8 and control-sequence state makes the result independent of where input is split.

Capacity and character-width settings are specified at terminal creation, and results report limit exhaustion, partial consumption, and output blocking. Scrollback capacity can also change after creation, unlike settings such as character width that are fixed at creation.
OSC/DCS/APC processing has limits and terminators; after consuming unknown or invalid parameters, parsing returns to normal input.

Core handles character clustering and intrinsic column widths; UI and renderers handle font faces, glyphs, shaping, bidirectional display, and pixel widths.
IME preedit also uses the public text-measurement API and replaces state only after UTF-8 validation and all allocations succeed.

## Managing text, images, and redraw information

Physical screen rows differ from logical lines joining wrapped rows.
Line IDs and revisions determine whether a position still refers to the same text after history eviction, line splitting/joining, and resizing.
Expired retained positions return STALE.

Text projection, selection, search, monitoring, and recognition share text positions.
Search resumes with bounded work; initial monitor scans use immutable snapshots.
Future events arriving during scanning are retained within a fixed capacity.
Content hashes are recalculated only for changed logical lines, without rereading every history cell each time.

Regular-expression recognition counts end-of-line scanning, UTF-8 decoding, boundary resolution, and NFA preparation against its work budget.
It does not retain an entire unmatched history as current matches.
See [Text workload measurements](../text-workload-measurements.md) for ways to inspect work and retained data.

Image decoding and registration are transactional and do not replace valid images before success.
Image resources and placements are separate; shared-resource revisions and drawing order invalidate the necessary regions.
Animation derives the current frame from monotonic time without drawing every missed frame.

## Input, host requests, and output ordering

Platform adapters convert OS events to common logical key, pointer, and IME input.
They choose between shortcuts, selection, links, and child-process input without delivering the same action twice.
Outgoing events are appended atomically, allowing the same operation to be retried on WOULD_BLOCK.

Host requests are taken from typed queues instead of calling OS APIs during feed.
Pending OSC 52 responses establish an output-order boundary so that later keyboard input cannot overtake them.
Request IDs, consumption, and completion order are checked, with completion occurring once.

Notifications assemble chunks in core, evaluate display conditions and frequency in policy code, and reach the OS through a provider.
An asynchronous dispatcher retains requests and ensures one completion even on cancellation or destruction.
External images similarly delegate host-side work.

## UI and rendering-resource management

The model synchronizes core changes, while multiple views retain independent decoration and rendering state.
Only one layout authority determines terminal grid dimensions and protocol cell geometry.
Additional views do not change that state and reject configurations they cannot relayout.

Frame processing distinguishes model changes, allocation, rendering resources, and presentation.
When a completed image can be retained, only presentation is repeated, without rerasterizing text.
Candidate and committed generations are separate so failures and retries cannot commit unfinished state.

GL context, D3D device, and output-target lifetimes and generations determine rendering-resource invalidation.
No release commands are issued to a lost context; required resources are recreated during recovery.
Device loss is distinct from temporary output blocking or missing allocation.

## Platform integration points

| Implementation | Ownership and integration points |
| --- | --- |
| X11/Win32 component | Child window or borrowed target, input adapter, renderer; the application owns the parent, event loop, and process |
| GTK widget | Connects GTK allocation and frames; stops callbacks during destruction, then the application releases the borrowed model and terminal |
| Native Wayland | Asynchronous subsurface and drawing buffers; display reads/flushes, parent role, and commits belong to the application |
| POSIX session | Returns nonblocking FD wait interests to the application |
| Win32 session | Connects synchronous ConPTY pipes to dedicated I/O processing; the application thread calling dispatch mutates terminal state |

For borrowed targets, full target dimensions are separate from the component viewport.
Pointer coordinates subtract the viewport origin, while IME candidates add it, keeping drawing and input aligned for nonzero origins.
Scrollbar width is also subtracted only once inside the component.

During destruction, stop native callbacks, pending clipboard and IME work, and rendering references before releasing owned resources.
Components do not destroy caller-owned displays, parents, models, or terminals.
The common [frontend fixture](../../../tests/frontend_component_fixture.c) and [conformance matrix](../../../tests/frontend_conformance_matrix.def) check shared conditions across implementations.

## References when making changes

Before changing a public API, use the [API index](../reference/api.md) to find its header, then compare the current implementation and functional tests.
See the [prototype evaluation](../search-capability-prototype.md) for search-transformation limitations and differences from public features, and [Testing](./testing.md) for checking changes.

Keep public-header documentation, documented examples, and test expectations consistent with the updated API.

[Documentation index](../index.md)
