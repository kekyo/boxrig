# Configuring terminal text and key I/O

[日本語](../ja/terminal-io.md)

You can adjust Enter and paste newlines, received newlines, and character encoding to match the communication peer.
Examples include devices that expect LF for Enter and peers using Shift_JIS.
These are boxrig core settings supplied when creating the terminal.
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
    config.enter_newline = BOXRIG_NEWLINE_LF;
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
| `enter_newline` | `BOXRIG_NEWLINE_TERMINAL` | Follow terminal mode 20, or force CR, LF, or CRLF |
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

`key_mappings` specifies length-delimited byte sequences for arrow keys and modifiers.
The table and bytes are copied at creation.
A zero length suppresses the key, and sequences containing NUL are valid.

Mappings require an exact match of the applicable modifier combination, excluding Caps Lock and Num Lock.
Duplicate mappings are invalid.
Limits are 64 entries, 4096 bytes per entry, and 65536 bytes in total.

Mappings apply to legacy key input and take precedence over application cursor mode.
No Alt ESC prefix or character encoding conversion is added to the completed sequence.
Events selected for Kitty key reporting retain Kitty format.
Build the [key mapping example](../../examples/core/key_mapping.c) with `make example-key-mapping` to compare the default Up sequence with a mapping to `55 00`.

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
