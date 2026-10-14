# Integrating into GTK 3/4

[日本語](../../ja/platforms/gtk.md)

The GTK version of boxrig UI provides a terminal widget for GTK applications.
It renders terminal text and images and integrates selection, copying, and IME input into your GTK interface.
boxrig core manages terminal contents, while GTK handles widget allocation and native input.
Your application connects communication, model synchronization, and frame processing.

## GTK versions and required libraries

The build requires GTK 3.24 or later for GTK 3, or GTK 4.14 or later for GTK 4.
These are separate libraries with separate types; link the version matching the GTK used by your application.

| GTK | SDK installation | pkg-config | Public header |
| --- | --- | --- | --- |
| 3 | `make install-gtk3 PREFIX=...` | `boxrig-gtk3` | [gtk3.h](../../../include/boxrig/gtk3.h) |
| 4 | `make install-gtk4 PREFIX=...` | `boxrig-gtk4` | [gtk4.h](../../../include/boxrig/gtk4.h) |

See the [build guide](../building.md) for dependencies and how to use `PREFIX`.

## Place a widget

This GTK 4 function creates a terminal widget to add to an existing container.
It is an excerpt of the creation code, called after initializing GTK, the terminal, and the UI model.
After placing the widget and connecting frame processing as described below, it can display text and images fed into the terminal.

```c
#include <boxrig/gtk4.h>

boxrig_ui_status create_terminal_widget(boxrig_ui_model *model,
    BoxrigGtk4TerminalWidget **widget_out)
{
    boxrig_gtk4_terminal_widget_config config =
        boxrig_gtk4_terminal_widget_config_default();
    return boxrig_gtk4_terminal_widget_create(model, &config, widget_out);
}
```

The returned object has a floating reference.
Place it in a container for your GTK version and manage its references.
For GTK 3, replace the header and the `gtk4`/`Gtk4` portions of type and function names with their GTK 3 equivalents.
Keep the terminal and model alive longer than the widget.
Follow [GtkWidget's documentation](https://docs.gtk.org/gtk4/class.Widget.html) for GTK's allocation and lifetime rules.

See the [GTK 3 application](../../../examples/boxrig_term/gtk3/application.c) and [GTK 4 application](../../../examples/boxrig_term/gtk4/application.c) for actual widget placement.

## Connect frame requests and communication

The configuration's `frame_request_callback` notifies your application to schedule work on its main loop.
Coalesce repeated requests and handle them later instead of rendering recursively inside the callback.

In that processing, advance the session, then call `boxrig_ui_model_synchronize()` followed by `boxrig_gtk4_terminal_widget_frame()`.
GTK calling draw or snapshot alone does not synchronize core changes to the model.
`frame()` does not wait; it returns remaining work and scheduled animation information.

See the [GTK 4 event source](../../../examples/boxrig_term/gtk4/event_source.c) for connecting PTY readiness to the GTK main loop.

## Handle sizing and scrolling

Layout follows the area allocated by GTK.
Specify preferred grid dimensions with `set_preferred_grid()` and retrieve the actual layout with `get_layout()`.
Apply grid changes to the communication peer before reporting completion with `complete_grid_resize()`.

Use the widget's adjustment and the common scrollbar model to manage scroll position.
For an application-provided scrollbar, see the [GTK 3 example](../../../examples/gtk3/external_scrollbar.h) or [GTK 4 example](../../../examples/gtk4/external_scrollbar.h).
These are header examples to include in your application, not standalone programs.

## Choose Cairo or OpenGL

The default `render_policy` is Cairo.
There are settings to prefer OpenGL or require it.
Requiring OpenGL fails if the necessary rendering environment is unavailable.
You can exclude optional OpenGL support from the build with `GTK_OPENGL=0`.

Successful creation does not mean that renderer initialization has finished.
After the widget is realized and its first frame completes, check the actual backend with `get_render_state()`.
Continue processing frames and GTK events until a switch's `pending` state clears.
A failed switch may preserve the previous usable rendering backend.

## Input and destruction considerations

IME selection is independent of the rendering backend.
Set preferences with `ime_policy` and `candidate_policy`, then use `get_ime_state()` to check the actual input route and candidate display.
Ordinary terminal input and input to an application-owned editable document use separate connections.

Use `set_input_enabled(widget, false)` to inhibit user input temporarily.
It stops this widget's keys, IME commits, paste, and terminal mouse reports while selection, copy, history navigation, rendering, and terminal replies remain available.
Request resumption with `true`. Old IME and clipboard replies are discarded, and keys held across a stop remain suppressed until release.

If a started paste cannot enqueue its closing sequence because output is full, the stop is accepted and returns `BUSY`.
Keep dispatching regular frames while `get_input_state()` reports `STOPPING`, and wait for `DISABLED` before resuming.
Requests made from a callback during input processing also return `BUSY`, but leave the state unchanged.
`DISABLED` means the closing sequence has been queued in core, not delivered to the communication peer.
Already queued core output and input from other widgets sharing the terminal are outside this API's scope.

During destruction, remove scheduled frames and communication-side references, call `quiesce()` to stop new work, then destroy the widget.
Do not release the model and terminal first.
`quiesce()` cannot be undone.

[GTK 3 API](../../../include/boxrig/gtk3/terminal_widget.h), [GTK 4 API](../../../include/boxrig/gtk4/terminal_widget.h), [Documentation index](../index.md)
