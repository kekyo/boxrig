# Configuring terminal text and key I/O

[日本語](../ja/terminal-io.md)

You can adjust Enter and paste newlines, received newlines, and character encoding to match the communication peer.
Examples include devices that expect LF for Enter and peers using Shift_JIS.
These are boxrig core settings supplied when creating the terminal.
Enter, erase bindings, and arrow mappings can also be exchanged together at runtime.
All views of the same terminal share these I/O rules.
They are independent of UI colors and fonts.

## Receiving and sending

Receiving means passing bytes from the peer to `feed()`.
Sending means adding keys, committed text, pasted text, or other input to the output queue.
Sending a key alone does not change the screen.

The following function creates a terminal that sends LF for Enter, uses CRLF for paste, and treats received CRLF as one newline.
It shows configuration before connecting communication, without using UI.
Keep the terminal returned on success and release it with `boxrig_terminal_destroy()` when finished.

```c
#include <boxrig/terminal.h>

boxrig_status create_line_terminal(boxrig_terminal **terminal_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.enter_newline = BOXRIG_NEWLINE_LF;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
    return boxrig_terminal_create(&config, terminal_out);
}
```

Settings are copied at creation and survive RIS and DECSTR resets.
Changing your local `config` after creation does not update the terminal.

## Enter and paste newlines

| Setting | Default | Choices |
| --- | --- | --- |
| `input.enter_newline` | `BOXRIG_NEWLINE_TERMINAL` | Follow terminal mode 20, or force CR, LF, or CRLF |
| `paste_newline` | `BOXRIG_NEWLINE_LF` | Convert LF in pasted text to CR, LF, or CRLF |

For Enter with TERMINAL, mode 20 disabled produces CR and enabled produces CRLF.
Fixed settings do not change with mode 20.
Kitty key reporting and application-keypad Enter follow their respective protocols.

Paste does not accept TERMINAL.
The standard UI normalizes clipboard CR and CRLF to LF, and core converts them to the selected outgoing newline sequence.
Bracketed-paste start and end sequences are not converted.
Paste settings do not apply to `send_text()` either.

## Received CR and LF

The value prefix is `BOXRIG_RECEIVE_NEWLINE_`.

| Value | CR result | LF result |
| --- | --- | --- |
| `TERMINAL`, default | Start of the same line | Next line; also move to its start if mode 20 is enabled |
| `STANDARD` | Start of the same line | Same column on the next line |
| `LF` | Start of the same line | Start of the next line |
| `CR` | Start of the next line | Same column on the next line |
| `CRLF` | Start of the next line | Start of the next line, except that a pair with the immediately preceding CR counts once |

With CRLF, adjacent CR and LF count once even across separate `feed()` calls.
Any intervening byte, including NUL or ESC, makes them count twice.
This setting does not apply to VT, FF, IND, NEL, or control-string contents.

Build the [newline example](../../examples/core/newline.c) with `make example-newline`.
`build/examples/newline` shows outgoing sequences and the cursor position after receiving input.
It bypasses a PTY, letting you observe the behavior separately from OS newline conversion.

## Change key sequences

`input.key_mappings` specifies length-delimited byte sequences for arrow keys and modifiers.
The table and bytes are copied at creation.
A zero length suppresses the key, and sequences containing NUL are valid.

Mappings require an exact match of the applicable modifier combination, excluding Caps Lock and Num Lock.
Duplicate mappings are invalid.
Limits are 64 entries, 4096 bytes per entry, and 65536 bytes in total.

Mappings apply to legacy key input and take precedence over application cursor mode.
No Alt ESC prefix or character encoding conversion is added to the completed sequence.
Events selected for Kitty key reporting retain Kitty format.
Build the [key mapping example](../../examples/core/key_mapping.c) with `make example-key-mapping` to compare the default Up sequence with a mapping to `55 00`.

## Backspace, Delete, and runtime settings

`boxrig_terminal_config.input` uses `boxrig_input_config`, also accepted by
`boxrig_terminal_set_input_config()`. Start with `boxrig_input_config_default()`
and replace Enter, erase bindings, and arrow mappings together. The terminal
validates and copies everything before exchanging settings. Invalid values,
capacity errors, and allocation failures preserve the previous configuration.
Screen content, history, selection, protocol modes, and queued output survive
successful exchanges. Enter, erase bindings, and arrow mappings can change at
runtime; codec and paste/receive newline policies remain construction settings.

`input.backspace_binding` and `input.delete_binding` are independent choices:

| `BOXRIG_ERASE_` value | Backspace | Delete |
| --- | --- | --- |
| `AUTO` (default) | DEL (`0x7f`) | `ESC [ 3 ~` |
| `ASCII_BACKSPACE` | BS (`0x08`) | BS (`0x08`) |
| `ASCII_DELETE` | DEL (`0x7f`) | DEL (`0x7f`) |
| `DELETE_SEQUENCE` | `ESC [ 3 ~` | `ESC [ 3 ~` |
| `TTY` | Channel erase byte, or Auto when unavailable | Channel erase byte, or Auto when unavailable |

Ctrl exchanges BS and DEL; Alt prefixes a one-byte binding with ESC. A delete
sequence carries modifiers as `CSI 3 ; modifier ~` without an extra Alt prefix.
Keypad Delete follows Delete. Legacy press and repeat send bytes; release does
not. Kitty reports and application-keypad encoding retain protocol priority.

`boxrig_terminal_get_input_config()` returns current settings and a borrowed
mapping table, valid until the next successful exchange or terminal destruction.
That value can safely be passed back to the setter, which copies before releasing
the previous table. These operations require external serialization.

Use the frontend's `set_input_config()` on its owning event thread: GTK3/GTK4
widgets and X11/Wayland/Win32 components provide this operation, as do X11 and
Win32 input adapters. It returns `BOXRIG_UI_BUSY` while the addressed component
has a held key, queued input, IME, clipboard, or paste transaction. Continue normal
event processing and retry after the series ends. When multiple components share
a terminal, the host must ensure that all their input series have ended.

The [key-mapping example](../../examples/core/key_mapping.c), built with
`make example-key-mapping`, also prints bytes after changing arrows, Enter,
Backspace, and Delete at runtime. The [Japanese original](../ja/terminal-io.md)
includes a configuration example.

## Using the TTY erase byte

`BOXRIG_ERASE_TTY` uses the current erase byte supplied by the communication
channel owner. `boxrig_tty_erase.available` distinguishes an obtained `byte`
from unavailable information. Core performs no OS query: pass information with
`boxrig_terminal_set_tty_erase()` and inspect the last supplied value with
`boxrig_terminal_get_tty_erase()`. Initially unavailable, TTY uses the key's Auto
behavior until a value is supplied.

On POSIX, call `boxrig_posix_get_tty_erase(fd, &erase)` immediately before sending
a TTY-bound key and pass the result to core on both success and failure. It reads
the current `VERASE` using [tcgetattr()](https://pubs.opengroup.org/onlinepubs/9799919799/functions/tcgetattr.html).
Failure clears the information and returns -1 with `errno` set. Querying at key
time observes later `stty erase` changes. The GTK3, GTK4, X11, and Wayland example
drivers include this integration.

NUL and `_POSIX_VDISABLE` are available bytes, not missing values. Ctrl exchanges
only BS and DEL; other bytes are unchanged. Alt prefixes ESC. TTY information
survives settings exchanges and screen resets and can be refreshed during a
held-key series. The host must supply `available = false` on disconnect or
channel replacement so the next connection cannot reuse the previous value.

For SSH, Telnet, serial, or other channels, supply a byte only when the connection
layer knows it. Win32 PTY has no erase-query API and uses unavailable information
with Auto fallback. Run `make test-tty-erase` to exercise POSIX changes and the
actual bytes received by the PTY child.

## Character encodings and raw bytes

Choose `encoding` from the default `BOXRIG_ENCODING_UTF8` and `BOXRIG_ENCODING_SHIFT_JIS`.
With Shift_JIS, received characters are converted to Unicode, and outgoing UTF-8 text is converted to Shift_JIS.
Text in cells, search, selection, copying, and IME remains UTF-8.

Shift_JIS support follows the [WHATWG Encoding Standard](https://encoding.spec.whatwg.org/#shift_jis).
Invalid received data becomes U+FFFD, and unrepresentable outgoing characters become ASCII `?`.
This does not guarantee exact equivalence to other CP932 implementations or round trips for every character.
Include the mapping table's [license](../../data/encoding/whatwg/LICENSE) when redistributing it.

Pass complete UTF-8 to `send_text()`.
Invalid or incomplete text returns an error without being appended.
Use `send_bytes()` for already encoded sequences or custom control sequences.
Encoding conversion is not applied again to query responses or image payloads.

Build the [encoding example](../../examples/core/encoding.c) with `make example-encoding`.
The outgoing text “日本語” becomes `93 fa 96 7b 8c ea`, and received cells can be inspected as UTF-8.

## Capacity limits and end of input

`WOULD_BLOCK` when sending means you can retry after making room in the output queue.
`LIMIT_EXCEEDED` means one operation exceeds a limit.
The check uses length after newline or encoding conversion; an operation is never only partially appended.

When receiving, check how many bytes `feed()` consumed and retain the remainder.
Incomplete characters and control sequences carry over to the next call.
At end of input, call `finish_input()` to process any incomplete trailing character.
Destroying the terminal alone does not add that trailing character to the screen.

When using a [session](./sessions.md), it processes remaining input and finalization after transport closure before reporting `peer_closed`.

[Documentation index](./index.md)
