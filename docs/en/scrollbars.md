# Managing scroll positions and scrollbars

[日本語](../ja/scrollbars.md)

boxrig UI lets users scroll through earlier output and return to the current output position.
Use a component's built-in scrollbar or connect a scrollbar supplied by your application.
boxrig core manages the history range and current position, which can be queried and changed through UI.

## Manage the scroll position

This function returns an independently managed UI view to the current output position.
Pass an existing synchronized view and a pointer to a `bool` that receives whether it moved.
For a standard component scrollbar, continue with [Built-in scrollbar settings](#configure-the-built-in-scrollbar).

```c
#include <boxrig/ui.h>

boxrig_ui_status scroll_to_output(boxrig_ui_view *view, bool *changed_out)
{
    return boxrig_ui_view_apply_scrollbar_action(
        view, BOXRIG_SCROLLBAR_BOTTOM, 0, changed_out);
}
```

If the view was partway through history, it moves to the latest position.
Nothing changes if it is already there, has no history, or shows the alternate screen.
Check both the return value and `changed_out`.

`apply_scrollbar_action()` also synchronizes the model when it changes the position.
The application advances requested frame processing to draw the new position.
`get_scrollbar_state()` returns the following state.

| State | Meaning |
| --- | --- |
| `history_rows` | History row count after wrapping |
| `viewport_rows` | Number of visible rows |
| `viewport_offset` | Rows scrolled back from the current output position |
| `position` | Position with 0 as the oldest and `history_rows` as current output |
| `enabled` | Whether the current screen permits history scrolling |
| `revision` | A number updated when state changes |

The first five fields are in `state.model`.
Views of the same model share scroll position and revision.
Revision comparisons are valid only during the lifetime of that same model.

## Change scrollback capacity

The primary-screen history limit is measured in physical rows. Change it while running with `boxrig_terminal_set_scrollback_capacity()` and read the current value with `boxrig_terminal_scrollback_capacity()`.
Increasing capacity retains existing history; decreasing it immediately evicts the oldest rows. Passing zero clears history.
`BOXRIG_SCROLLBACK_CAPACITY_MAX` is the maximum accepted value. A limit or allocation failure leaves capacity, history, viewport position, and other state unchanged.
After changing core directly, synchronize each UI model in use before advancing frame processing.

## Configure the built-in scrollbar

Visibility policies are ALWAYS, AUTO, and NEVER.
The constant prefix is `BOXRIG_UI_SCROLLBAR_`.
The default ALWAYS reserves space at all times and displays a disabled bar when scrolling is unavailable.
AUTO shows it only when history scrolling is possible; NEVER returns its width to the text area.

Set the policy with each component's `set_scrollbar_visibility()` and retrieve requested and actual placement with `get_scrollbar_visibility()`.
`pending` means allocation or grid-size changes are unfinished.
Hiding the bar does not erase history, but it may change the text area's column count.

On X11 and Win32, the creation configuration's `scrollbar_presentation` selects a native child scrollbar or an in-surface scrollbar.
When created with NONE, changing visibility policy alone does not add a built-in scrollbar.
Wayland defaults to an in-surface scrollbar.

For GTK, create the standard container with `create_scroll_container()` before adding the terminal widget to a parent.
The application takes the returned floating reference and places the container.
Use boxrig settings instead of independently overriding GTK visibility policy after creation.

## Connect an application scrollbar

Read state during frame processing and update the bar's range and position only when the revision changes.
Synchronize the model first if core was modified directly.
Reading state alone neither changes position nor consumes redraw information.

Translate bar operations to LINE_UP/DOWN, PAGE_UP/DOWN, TOP, BOTTOM, and POSITION.
POSITION is an absolute position with 0 as the oldest.
Other operations do not use the position argument.

Prevent notifications caused by updating the bar from becoming new terminal actions.
The integration examples for [X11](../../examples/x11/external_scrollbar.h), [Win32](../../examples/win32/external_scrollbar.h), [GTK 3](../../examples/gtk3/external_scrollbar.h), [GTK 4](../../examples/gtk4/external_scrollbar.h), and [Wayland](../../examples/wayland/external_scrollbar.h) include this handling.

## Change allocation and scale

The width reserved for a built-in scrollbar uses pixels on X11 and Win32, and logical pixels on GTK and Wayland.
Native Wayland defaults to 14 logical pixels.
For an external scrollbar, your application manages allocation and scale.

A conflicting change returns `BUSY` without accepting the new policy.
Once accepted, complete normal events, frames, and grid-size changes.
Hiding a bar during a drag ends the drag and does not forward the remaining release event as terminal input.

## Try it in the demo

Start the [bundled terminal](./boxrig_term.md) with a setting such as `BOXRIG_SCROLLBAR=auto`.
When explicitly configured, Ctrl+Shift+F9 cycles through ALWAYS, AUTO, and NEVER.
When unset, it uses ALWAYS and does not consume this shortcut.

For a custom-drawn thumb, use `boxrig_scrollbar_calculate_thumb()` and `position_from_thumb()`.
See the [public header](../../include/boxrig/ui/scrollbar.h) for definitions and state meanings.

[Documentation index](./index.md)
