# Connecting input and IME

[日本語](../ja/input-ime.md)

Standard boxrig UI components let users operate a terminal with the keyboard and mouse and display IME preedit and candidates.
Components receive OS input, and boxrig core converts it into outgoing sequences for the terminal's current mode.
Your application sends those sequences to the peer through a session or another connection.

For a custom input route, distinguish ordinary keys, committed text, and pasting.
This guide covers normal terminal input first, then integration with editable documents whose text belongs to the application.

## Receive keys, mouse events, and pasted text

Pass independently received keys to core as `boxrig_key_event` and mouse events as `boxrig_mouse_event`.
The following function adds the committed text “日本語” to an existing terminal's output queue.
Call it from custom input handling; do not call it again for input already handled by a standard component.

```c
#include <boxrig/terminal.h>

boxrig_status commit_text(boxrig_terminal *terminal)
{
    const char text[] = "日本語";
    return boxrig_terminal_send_text(terminal, text, sizeof(text) - 1);
}
```

On success, the text is added to the output queue.
It does not write “日本語” directly to the screen; the screen reflects the peer's response after receiving it.
On `WOULD_BLOCK`, advance output and retry the same committed input once.

Pasting uses BEGIN, CHUNK, and END stages.
Each chunk must contain complete UTF-8 characters.
See [Terminal I/O settings](./terminal-io.md) for native copying and pasting and newline settings.

## Display preedit and candidates

Preedit is overlaid as a composition.
Writing it into terminal cells would make it indistinguishable from output received from the peer.
Standard components also adjust cursor display and candidate placement for the IME.

Candidate policies are AUTO, SYSTEM, and CLIENT.
Even if CLIENT is preferred, in-application display is unavailable when the input route cannot supply candidates.
Check diagnostics for the requested policy, actual candidate owner, and reason.

## Choose an input route

| Platform | Main input routes | Information to check |
| --- | --- | --- |
| X11 | XIM, direct IBus | Connection state, input owner, candidate owner |
| GTK 3/4 | GtkIMContext, direct IBus when applicable | Route and reason from `get_ime_state()` |
| Native Wayland | Compositor text-input, direct IBus | Context input route and component diagnostics |
| Win32 | IMM, TSF integration for editable documents | Input capabilities, reconversion and editing availability |

In GTK, choose the rendering backend and IME policy separately.
Explicit GTK modules, the actual display, and runtime capabilities also affect selection.
Do not infer that direct IBus is active from a policy name alone.
In particular, GTK 4/X11 can use GTK's IBus module even with the IBus policy.

## Supply an application-owned editable document

The communication peer owns normal terminal contents, so do not substitute terminal-cell mutations for IME deletion or replacement requests.
For editor use where your application owns the text, publish a text window, cursor, selection, document ID, and revision through the view's editable document.

`boxrig_ui_editable_document_publish_context()` copies the surrounding text.
Positions are absolute UTF-8 byte offsets; cursor and anchor must lie on character boundaries.
Do not include preedit in the document text.
Changing only the selection still updates the input-state generation separately, even when text is unchanged.

A small runnable example is included in the [step-by-step editable document section](./step-by-step/step-by-step-ui.md).
The [public header](../../include/boxrig/ui/editable.h) specifies capacities, boundaries, and when failure preserves the previous state.

## Handle commits and reconversion

For synchronous editing, the application's owner callback applies the requested replacement exactly once and accepts the new revision.
On failure, it must not change the text.
Do not wait for I/O, run a nested event loop, or modify or destroy the view inside the callback.

For reconversion, fix the target document ID, revision, and selection before advancing the request.
The application takes the returned replacement request and completes it as applied, rejected, or stale.
Do not apply a request to the current document if the text or input state changed in the meantime.

Sending to a normal terminal and replacing text in an editable document have different commit destinations.
Do not deliver one commit to both.

On Win32, ordinary IME commits go to the PTY even when surrounding-document publication or selection reconversion is connected.
Registering the common synchronous edit callback alone does not change that destination.
See the [IME support table](./reference/platform-support.md#ime-notifications-clipboard-and-accessibility) for differences between input routes.

## Check platform-specific limitations

Retrieving surrounding text, synchronous editing, and reconversion are separate capabilities.
Check that the native API, IME, input route, and application edit owner all support the operation.

Start with [Compatibility](./compatibility.md#ime-surrounding-text-and-synchronous-editing) for requirements and the [bundled terminal](./boxrig_term.md) for runtime selection.
When shutting down with pending input or clipboard operations, disconnect notification targets according to [Ownership and shutdown](./architecture.md#ownership-and-shutdown).

[Documentation index](./index.md)
