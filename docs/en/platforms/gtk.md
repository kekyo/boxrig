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

## Read terminal text through assistive technology

The GTK 3 widget implements `AtkText`, and the GTK 4 widget implements `GtkAccessibleText`.
Text publication is disabled by default. Applications using screen readers or automated text retrieval must set `accessibility_enabled = true` in the widget creation configuration.
Use `set_accessibility_enabled()` to change it at runtime and `get_accessibility_enabled()` to read the setting.
Disabling withdraws previously published text and stops snapshot generation and change notifications. Enabling publishes the current viewport on the next `frame()`; synchronized output waits until its batch completes.
Standard GTK accessibility for menus and buttons is unaffected.
When enabled, the widget reuses published text while its content, attributes, geometry, selection, and caret remain unchanged. Blinking and IME preedit overlays alone do not rebuild terminal text.

AT-SPI exposes the visible text, a caret corresponding to the terminal cursor, per-view selections, and character attributes.
Offsets count Unicode scalars. Wide-character continuation cells do not add characters, while combining characters count their individual scalars.
Character extents exposed by GTK 3 cover the cells occupied by the grapheme.

Contents include visible rows, rather than the entire scrollback history.
They follow history navigation, alternate-screen switches, and reflow after width changes.
Soft wraps do not add newlines; hard breaks and empty rows contribute LF.
Unwritten trailing cells are omitted, while explicitly printed spaces and indentation are retained.
IME preedit text is excluded.

Processing `frame()` publishes text, caret, and selection changes.
Synchronized output retains the previous contents, and blinking alone does not repeat text notifications.
Removal callbacks can read the old contents; insertion callbacks can read the new contents.
A cursor outside the viewport maps to the end of the text.
Assistive-technology requests to move the caret or change selections are unsupported and do not generate child-process input.

Each published snapshot of text and position information is limited to 16 MiB.
Exceeding that limit makes `frame()` return `BOXRIG_UI_LIMIT_EXCEEDED` and preserves the previous contents.
See [AtkText](https://docs.gtk.org/atk/iface.Text.html) and [GtkAccessibleText](https://docs.gtk.org/gtk4/iface.AccessibleText.html) for the retrieval API contracts.

GTK 4 supports the text, caret, selection, attribute, and change-notification contracts available in the minimum version, 4.14.
Geometry queries introduced in 4.16 and caret/selection mutation introduced in 4.22 are not provided.
AT-SPI text retrieval and Orca speech-text generation have been verified on GTK 4.14.
Attribute-only changes are available through queries, but GTK 4.14 has no public notification API dedicated to attributes.
See the [versioned GtkAccessibleText API](https://docs.gtk.org/gtk4/iface.AccessibleText.html) for available operations and notifications.

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
