# Checking terminal behavior with emitted control sequences

[日本語](../ja/boxrig_emitter.md)

`boxrig_emitter` lets you try colors, cursors, images, keyboard input, and other features individually and inspect terminal display and responses.
It is a CLI tool that emits test text, control sequences, and queries, and also runs in terminals other than boxrig.
Start with the short examples on this page to see display results.
For byte interpretation and support scope, follow the specification guides at the end.

## Build and run

Build the Linux version with `make native` and Windows versions with `make win32`.
To build only the tool, target its output directly.

```sh
make build/boxrig_emitter/posix/boxrig_emitter
./build/boxrig_emitter/posix/boxrig_emitter --help
```

Windows binaries are `build\boxrig_emitter\win32\amd64\boxrig_emitter.exe` and `i686\boxrig_emitter.exe`.
They need not match the terminal's ABI, but must be executable on the OS.

## Your first check

This example checks backspace in boxrig's X11 terminal.
The final C in `ABC` disappears, leaving `AB`.
First build `boxrig_term` using the [build guide](./building.md#build-for-linux).

```sh
./build/boxrig_term/x11/boxrig_term -- \
    ./build/boxrig_emitter/posix/boxrig_emitter -t backspace
```

The case prints `ABC`, then erases its trailing C with BS and a space.
Compare the `EXPECT` description with the display between `BEGIN` and `END`, checking that `AB` remains.
Add `--no-hold` to skip waiting for Enter at the end.

```sh
./build/boxrig_emitter/posix/boxrig_emitter -t backspace --no-hold
```

## Choose a case

These commands list available cases, describe the Kitty graphics case, and run it.
On a supported terminal, they demonstrate image transfers and placements.

```sh
./build/boxrig_emitter/posix/boxrig_emitter --list
./build/boxrig_emitter/posix/boxrig_emitter --describe kitty-image-static
./build/boxrig_emitter/posix/boxrig_emitter -t kitty-image-static
```

`--list` lists names, categories, and I/O kinds; `--describe` shows expected results and actions.
Supply one name to `-t`.
The screenshot below shows `kitty-image-static` in `boxrig_term`.

![Kitty graphics example](../../images/kitty-image-static.png)

## Check output and responses

Distinguish emitting output from the terminal interpreting it correctly.

`output` cases are observed visually, `round-trip` cases read terminal replies, and `interactive` cases require user input such as keys.
`frontend` cases exercise actions that control sequences alone cannot produce, such as resizing and IME preedit.

`--no-hold` skips the final wait.
It does not automate the queries or interactive actions themselves.
Saving output to a file alone cannot verify terminal replies or rendering.
Existing automation in [tests/test_emitter.c](../../tests/test_emitter.c) checks actual output and core results.

Cases marked `host-effect` also affect state outside the display, such as titles, the clipboard, and notifications.
`clipboard-write` changes clipboard contents to `boxrig clipboard fixture`; `clipboard-read` returns contents to the child process if permitted.
The bundled terminal denies reads by default.
Notifications require `BOXRIG_ALLOW_NOTIFICATIONS=1`, and external images require `BOXRIG_ALLOW_GRAPHICS_EXTERNAL=1`.

Permission does not make an unsupported feature succeed.
For example, the native Wayland bundled terminal has no OSC 52 provider connected.
On Windows, ConPTY's re-emission of control sequences also affects results.
Check [Platform support](./reference/platform-support.md) as well.

## Case list

The executable's `--list` is the current list.
All cases are grouped by purpose below.

| Purpose | Case names |
| --- | --- |
| Transport | `transport-bytes` |
| Text | `text-ascii`, `text-cjk`, `text-combining`, `text-long-grapheme`, `text-ligatures`, `text-complex-scripts`, `text-bidi`, `text-emoji`, `text-ambiguous-width`, `text-invalid-utf8` |
| C0/ESC | `backspace`, `c0-controls`, `cancel-recovery`, `esc-indexing`, `character-sets`, `terminal-reset` |
| Cursor/editing | `cursor-movement`, `character-editing`, `line-scrolling`, `tab-stops`, `erase-display`, `erase-line`, `selective-erase`, `margins`, `save-restore-soft-reset`, `cursor-style-protection` |
| Modes | `standard-modes`, `private-modes`, `alternate-screen-modes`, `title-stack` |
| Decorations/colors | `sgr-attributes`, `sgr-ansi-colors`, `sgr-extended-colors`, `sgr-underline-styles` |
| OSC | `osc-titles`, `osc-working-directory`, `osc-hyperlink`, `osc-palette`, `osc-dynamic-colors`, `clipboard-write`, `desktop-notification`, `desktop-notification-options` |
| Images | `kitty-image-static`, `kitty-image-external`, `kitty-animation-direct`, `kitty-animation-client`, `kitty-animation-loop`, `kitty-animation-compose` |
| Queries | `query-cursor-position`, `query-device-status`, `query-device-attributes`, `query-modes`, `query-decrqss`, `query-colors`, `clipboard-read`, `notification-query`, `query-kitty-graphics`, `query-kitty-keyboard` |
| Interactive input | `input-key`, `input-application-cursor`, `input-application-keypad`, `input-kitty-keyboard`, `input-paste`, `input-focus`, `input-mouse`, `input-ime-commit` |
| Window operations | `frontend-resize`, `frontend-scrollback`, `frontend-font-zoom`, `frontend-ime-preedit` |
| Recovery from unsupported input | `safe-no-op`, `unsupported-recovery` |

## Specification guides

See [Control sequences](./reference/terminal-sequences.md) for sequences and reply values, [Graphics specification](./reference/graphics.md) for image formats and limits, and [Platform support](./reference/platform-support.md) for OS, IME, and rendering conditions.
Settings for boxrig_term are in the [bundled terminal guide](./boxrig_term.md).

[Documentation index](./index.md)
