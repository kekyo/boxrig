# Blinking text and cursors

[日本語](../ja/blinking.md)

boxrig UI lets you enable text and cursor blinking and set their intervals independently.
Settings belong to each view, so the same terminal can appear in a blinking view and a steady view simultaneously.

For standard components, an animation runtime shared by the same UI loop manages time and the next notification.
Custom renderers can also advance time manually through an API.

## Enable blinking

This function sets the cursor's visible interval to 600 ms and hidden interval to 400 ms.
Pass a UI view already attached to an animation runtime and continue normal event and frame processing.
See [Connecting standard components](#connect-standard-components) for runtime setup or [Connecting a custom wait loop](#connect-a-custom-wait-loop) for manually advancing time.

```c
#include <boxrig/ui.h>

boxrig_ui_status set_cursor_timing(boxrig_ui_view *view)
{
    boxrig_ui_blink_config blink = boxrig_ui_blink_config_default();
    blink.cursor.visible_milliseconds = 600;
    blink.cursor.hidden_milliseconds = 400;
    return boxrig_ui_view_set_blink_config(view, &blink);
}
```

If the terminal and style request a blinking cursor, it switches between visible and hidden at these intervals.
For text, SGR 5 selects slow blink, SGR 6 rapid blink, and SGR 25 disables blink.
Disabling blinking makes text or cursors steady but does not override explicit hiding requested by the terminal.

## Intervals and focus behavior

Text and cursor blinking are enabled by default.
Slow blink and the cursor use 500 ms for each phase; rapid blink uses 150 ms for each.
Each interval must be positive, and each pair's sum must fit in `uint64_t`.

The cursor returns to its visible phase on movement, local input, or focus restoration.
Losing focus makes it steady.
Text blinking continues independently of focus but stops when the view is hidden.
Showing the view again restarts from the visible phase.

During text's hidden phase, glyphs, underlines, and strikethroughs disappear.
Backgrounds, images, selection and search outlines, and copied text do not change.

## Connect standard components

| Platform | Connection |
| --- | --- |
| X11 | Create an adapter with `boxrig_x11_animation_poll_create()` and supply it through the configuration's `animation_runtime` |
| Wayland | Use `boxrig_wayland_animation_poll_create()` in the same way |
| GTK 3/4 | Widgets attach automatically to the creating thread's main context |
| Win32 | Components attach automatically to notifications on the UI thread that created them |

On X11 and Wayland, creation with blinking enabled but no runtime returns `UNAVAILABLE`.
If blinking is unnecessary, as in the initial fixed-text example, disable both text and cursor blinking in the creation configuration.

Retrieve the runtime from a poll adapter with `boxrig_ui_animation_poll_runtime()`.
Immediately before waiting, use `prepare_wait()` to apply the next deadline to the timeout; process it with `dispatch()` after waiting.
Share an adapter across components and release it after destroying all those components.

## Connect a custom wait loop

A custom scheduler implements time retrieval, one-shot scheduling at absolute deadlines, and cancellation.
Do not dispatch synchronously during scheduling; deliver notifications to the owning UI thread.
Return the scheduled generation unchanged, and check for a live registration before handling a canceled notification.

The [manual clock example](../../examples/ui/animation_manual.c) supplies the same monotonic time to core and the view and checks their state.
Without a shared runtime, retrieve deadlines for both view blinking and core image animation/synchronized output, and use the earliest deadline for waiting.

```sh
make build/tests/animation-scheduler-example build/tests/animation-manual-example
build/tests/animation-scheduler-example
build/tests/animation-manual-example
```

The [shared scheduler example](../../examples/ui/animation_scheduler.c) covers two views, long pauses, hiding, and stale notifications.

## Clock lifetime and failures

Use monotonically increasing absolute milliseconds.
An unchanged time is a no-op; time moving backward is an error.
After a long pause, processing advances to the current phase rather than replaying every missed blink.

Updating a terminal or view attached to a shared runtime through another runtime or a manual clock returns `BUSY`.
If scheduling fails during a setting change, the previous settings and schedule remain.
Check runtime status and component diagnostics for scheduling failures during operation.

During shutdown, disconnect notification targets and detach views before releasing the runtime.
Do not deliver delayed notifications to destroyed pointers.

[Animation API](../../include/boxrig/ui/animation.h), [Blink API](../../include/boxrig/ui/blink.h), [Documentation index](./index.md)
