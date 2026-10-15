# Integrating into native Wayland

[日本語](../../ja/platforms/wayland.md)

The native Wayland version of boxrig UI displays a terminal as a child of an application-owned surface.
It combines boxrig core's terminal contents with Wayland rendering and input handling.
Use it to build a Wayland event loop without GTK.
Your application manages the display, dedicated event queue, parent surface, and toplevel configuration.

## Required protocols and libraries

The build uses Wayland, xkbcommon, Pango/Cairo, and the repository's pinned wayland-protocols.
Install an SDK with `make install-wayland PREFIX=...` and link using the `boxrig-wayland` pkg-config file.
See the [build guide](../building.md) for details.

Protocols available at runtime depend on the compositor.
The context remains initializing until mandatory capabilities are available; diagnostics identify unsupported input and scaling features.
Protocol definitions are also available in the [pinned wayland-protocols](../../../deps/wayland-protocols).

## Prepare surfaces and a component

The following function creates a child surface for displaying a terminal inside an existing parent surface.
Along with the parent surface, terminal, and UI model, prepare a context containing the Wayland connection information.

Passing the connection and dedicated queue to `boxrig_wayland_context_create()` starts asynchronous registry discovery.
The creation function itself performs no roundtrip or event reads.
Process events until the context is READY before calling the creation excerpt below.
Pass the [animation runtime](../blinking.md) used by the same loop as `animations`.

```c
#include <boxrig/wayland.h>

boxrig_ui_status create_terminal_surface(boxrig_wayland_context *context,
    struct wl_surface *parent, boxrig_ui_model *model,
    boxrig_ui_animation_runtime *animations,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    config.animation_runtime = animations;
    return boxrig_wayland_terminal_component_create(
        context, parent, model, &config, component_out);
}
```

The component owns its child surface and subsurface.
It borrows the parent surface, context, and model.
The current API uses a child surface; this creation API does not also render to arbitrary existing surfaces.

## Wait for events and render

After dispatching the dedicated queue, call `boxrig_wayland_context_dispatch()` to process received notifications.
Synchronize communication-induced changes to the model before calling the component's `frame()`.

The application flushes output to the display and reports success to `boxrig_wayland_context_note_flushed()`.
See the [actual event loop](../../../examples/boxrig_term/wayland/event_loop.c) for Wayland waiting, including read preparation and cancellation.
It combines display, PTY, clipboard, IME, key repeat, and blink deadlines into one wait.

When `frame_pending` is true but `additional_dispatch` is false, processing may be waiting for a buffer release or frame callback.
Repeated rendering during that wait makes no progress.

## Size, scale, and decorations

Positions and dimensions passed to `allocate()` are surface logical coordinates.
The component prepares a physical-pixel render target matching the output scale.
It handles integer scaling and fractional scaling when available.

Changing child placement can require a parent commit.
Follow the request returned by `take_host_action()`, commit the parent in your application, and return the same revision to `complete_parent_commit()`.
boxrig does not commit a borrowed parent on its own.

Toplevel decorations are the application's responsibility.
The bundled demo uses libdecor where available, but libdecor is not required to use a boxrig component.

## Input and the clipboard

The default AUTO policy prefers compositor input when text-input-v3 version 2 is available and the candidate policy is not CLIENT.
Otherwise, it discovers IBus and fcitx 5/4.
Applications can also explicitly select an input route.
Check context and component diagnostics for surrounding text, candidate display, and the selected input route.

Retrieve clipboard transfer FDs with `prepare_wait_sources()` and pass their readiness to `dispatch_wait_sources()`.
The FDs are borrowed; do not close them.
Refresh the wait sources after Wayland or transfer dispatch.
Direct IME connections also use `prepare_ime_poll()` and `dispatch_ime_poll()`.

Call `boxrig_wayland_terminal_component_set_input_enabled(component, false)` to inhibit input from that component to the terminal.
Inhibition and resumption each advance the input generation, invalidating pending keys, IME commits, incoming clipboard transfers, and unstarted pastes.
Keys held across inhibition remain suppressed until released.
Selection, copy, history navigation, local key and mouse actions, rendering, and terminal protocol replies continue.

If an active paste terminator cannot yet be submitted, inhibition returns `BUSY` and the state becomes `STOPPING`.
Continue context dispatch while processing core output normally.
Resume only after `get_input_state()` reports `DISABLED`.
Completion means the required terminator has entered the core; transport draining remains the host's responsibility.
Calls from an input-processing callback also return `BUSY`, preserving the current state.
Refresh wait sources after inhibition because transfer FDs may have become invalid.

## OpenGL selection and shutdown

The OpenGL version uses `boxrig-wayland-ogl` and the [dedicated creation API](../../../include/boxrig/wayland/opengl_component.h).
Responsibilities for common input, allocation, and parent commits remain the same.

During shutdown, stop wait registrations and callbacks, destroy the component, and release the context.
Then release the application-owned surfaces, queue, and display.
The library does not disconnect a borrowed display.

[Wayland public API](../../../include/boxrig/wayland.h), [Running the demo](../boxrig_term.md), [Documentation index](../index.md)
