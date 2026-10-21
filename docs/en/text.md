# Retrieving, selecting, and searching terminal text

[日本語](../ja/text.md)

You can save terminal contents to a file, search text, and detect particular output.
These operations work with boxrig core alone and do not require a display.
With boxrig UI, you can also combine mouse selection, search highlighting, and recognition-result decorations.

Use snapshots to save contents, exact search for user searches, and monitors or recognizers to detect output.
First, here is an example that extracts text from an existing terminal.

## Retrieve text

This function extracts text from the primary screen and history of an existing terminal.
Call it after your application has passed received data to `boxrig_terminal_feed()`.
For example, the terminal in the [README core example](../../README.md#interpret-terminal-output-without-a-display) contains `Hello` in its text, without the red-color control sequence.
The caller owns the snapshot returned on success.

```c
#include <boxrig/terminal.h>

boxrig_status snapshot_text(boxrig_terminal *terminal,
    boxrig_text_snapshot **snapshot_out)
{
    boxrig_text_snapshot_options options = {
        .screen = BOXRIG_TEXT_SCREEN_PRIMARY,
        .range = NULL,
        .byte_capacity = 1024u * 1024u,
        .line_capacity = 12000
    };
    return boxrig_terminal_create_text_snapshot(terminal, &options, snapshot_out);
}
```

`boxrig_text_snapshot_data()` returns UTF-8 bytes and their length.
Use the length when saving; do not assume NUL termination.
Release the snapshot with `boxrig_text_snapshot_destroy()` when finished.
Terminal updates after creation do not change the snapshot.

Exceeding capacity returns `LIMIT_EXCEEDED`, not a successful partial document.
Borrowed results such as `get_cell()`, used to read screen cells temporarily, remain valid only until the next terminal update.

## Display rows and logical lines

Rows wrapped because of a narrow terminal are treated as one logical line.
A normal newline separates logical lines.
Snapshots and search add no characters at wraps and project logical-line boundaries as LF.

Positions include the screen kind, logical-line ID, column, and revision, rather than only an on-screen row number.
The primary screen includes history; the alternate screen is a separate document.
Arbitrary positions inside wide or combining characters cannot be selection boundaries.

## Selection and copying

UI views provide APIs for normal and rectangular selection.
Start with `begin_selection()` or `begin_selection_with_options()` and update the endpoint with `update_selection()`.
Using hit-test results based on text layout keeps interactions aligned with visual appearance even for bidirectional text.

Retrieve copied text with `boxrig_ui_view_create_selection_snapshot()`.
Rectangular selections are projected per display row, without duplicating wide characters, and with LF between rows.
Changing selection appearance does not change the extracted text.

Updating a rectangle to equal column boundaries retains its anchor as an empty selection.
The update succeeds, and moving left or right extends from the same anchor again.
Row and range queries return zero items, and there are no visual ranges.
Copying returns `BOXRIG_UI_UNAVAILABLE`; leave the existing clipboard unchanged.
Calling `clear_selection()` also removes the retained anchor.

Passing a selection to the OS and handling an OSC 52 request from a terminal application are separate operations.
See [Host integration](./host-integration.md) for permission and completion handling of the latter.

## Search for exact matches

In core, create a search with `boxrig_terminal_create_text_search()` and advance it by passing a positive work budget to `boxrig_text_search_resume()`.
In UI, `boxrig_ui_view_begin_search()` and `resume_search()` manage search state and result highlighting.

The query matches UTF-8 bytes exactly.
You can specify forward or backward search, a starting position, and whether to wrap past that position once.
Case folding, Unicode normalization, and arbitrary regular-expression search are not part of this search API.

Results are “in progress,” “match,” or “not found.”
While in progress, return control to the event loop and resume later.
If `reveal_search_result()` moves the viewport after a match, synchronize the model before rendering.

## Monitor text occurrences

Pass a string, the screens to monitor, and whether to include existing contents to `register_text_monitor()`.
`FUTURE_ONLY` reports only matches produced by text changes after registration.
Viewport movement or rewrapping unchanged text does not create a new occurrence.

`EXISTING_AND_FUTURE` first searches a fixed snapshot of the text at registration.
Advance scanning with a work budget passed to `dispatch_text_monitors()`; existing matches are published in primary-then-alternate document order, followed by matches created meanwhile in mutation order.
Events distinguish existing matches from new occurrences.
The application peeks events and consumes them after processing.
Parsing does not invoke application callbacks.

Terminal input and screen updates continue even when capacity limits cause notifications to be lost.
`text_monitor_needs_rescan()` becomes true, and `RESCAN_REQUIRED` is delivered when the notification queue has room.
Process remaining events and call `rescan_text_monitor()` to scan the current text again.
The rescan requirement remains until successful.
A rescan recovers current matches; it cannot recover text that appeared and disappeared while notifications were lost.

Set monitor capacities in `boxrig_terminal_config` when creating the terminal.

| Resource | Default limit | On overflow |
| --- | --- | --- |
| Registrations / total pattern bytes | 32 / 256 KiB | Reject registration without leaving a partial registration |
| Snapshot of existing contents | 8 MiB, 262,144 logical lines | Reject registration or rescan without returning partial text as success |
| New occurrences retained during initial scanning | 1,024 per monitor | Require a rescan |
| Application notification queue | 4,096 per terminal | Require a rescan |
| Contents recorded from one text mutation | 1 MiB, 4,096 logical lines | Continue screen updates and require a rescan |

## Connect recognition results to decorations and actions

`register_text_recognizer()` is a bounded regular-expression API for detecting text such as URLs.
It accepts the following syntax.

| Syntax | Meaning |
| --- | --- |
| UTF-8 characters, `.` | Literal characters, or any one Unicode scalar value |
| Character classes, ranges, negation | Match the specified character set |
| Groups, alternatives | Group an expression or choose between expressions |
| `?`, `*`, `+`, `{m}`, `{m,n}` | Repetition; finite upper bounds also have a limit |
| `^`, `$` | Start and end of a logical line |
| `\n`, `\r`, `\t`, `\\`, `\u{scalar}` | Newline, carriage return, tab, backslash, or the specified scalar value |
| `\d`, `\s`, `\w` | ASCII digit, whitespace, or word characters |

Patterns that can match an empty string, backreferences, lookahead, lookbehind, recursion, and locale-dependent character classes are unsupported.
No Unicode normalization or case folding is performed.
If registration fails, inspect the diagnostic and byte position in the pattern.

Recognition runs per logical line, joining wrapped rows but not crossing normal newlines.
It selects the first match from the left and the longest match at the same starting position.
Matches do not overlap, and both endpoints must be grapheme boundaries that do not split combining characters.

At registration, specify limits for compilation, line length, result counts, and retained data.
The defaults from `boxrig_text_recognizer_limits_default()` allow up to 1 MiB and 1,048,576 columns per line, 4,096 matches per line, and 65,536 matches per recognizer.
Split work with `resume_text_recognizers()` and retrieve events and current results.

Apply UI text annotations to recognized ranges to add colors or underlines.
On `RESCAN_REQUIRED`, remove that recognizer's decorations and rescan.
`SCOPE_INCOMPLETE`, caused by reaching line-length or match-count limits, is not a complete result.
If a result query returns `INCOMPLETE` or `STALE`, do not apply previous results to current text.
On a click, query the current recognition result before executing the application's action.

Exact search, text monitoring, and regular-expression recognition are separate APIs.
The earlier transformation-search prototype cannot be used as a current search feature.

## Positions after updates and work budgets

Snapshots retain the text captured at creation, but terminal updates can affect search cursors and ranges.
If `STALE` is returned, do not apply an old position directly to current text.
Choose retrieval, a new search, or revalidation with `rebase_text_range()` as appropriate.

Work budgets are integers representing scanned cells, bytes, or similar units.
Split work into small budgets while also handling input and rendering.
Distinguishing capacity exhaustion, invalid ranges, and stale results lets you process long histories without blocking the UI.

[Core API](../../include/boxrig/terminal.h), [UI selection and search API](../../include/boxrig/ui/view.h), [Documentation index](./index.md)
