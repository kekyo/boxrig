# Connect a character encoding converter to a terminal

[日本語](../ja/encoding-hooks.md)

boxrig uses UTF-8 by default. An application talking to a CP932 instrument or an ISO-2022-JP peer can register a converter per terminal to translate between the peer's encoding and UTF-8. Screen text, search text, and strings passed to `boxrig_terminal_send_text()` use UTF-8.

## Choose what to convert

Incoming data contains characters to display and control sequences for operations such as cursor movement. `ESC` denotes the escape byte (`0x1b`) used to introduce a control sequence.

| Format used by the peer | Setting | Data reaching the converter |
| --- | --- | --- |
| UTF-8, or data the connection already converts to UTF-8 | Leave the hooks unregistered | Pass UTF-8 to `boxrig_terminal_feed()`. At end of input the built-in decoder displays `�` (U+FFFD) for an incomplete character |
| Text in CP932 or a similar encoding, terminal controls in ASCII | `BOXRIG_ENCODING_FIELDS` | Decode screen text and titles separately to UTF-8. Encode outgoing text and paste content for the peer |
| ISO-2022-JP, which uses ESC to switch character sets, or UTF-16LE, which also encodes control sequences | `BOXRIG_ENCODING_STREAM` | Decode the entire received stream before parsing controls. Encode terminal-generated key sequences, paste delimiters, and replies on output |

Field decoders produce printable UTF-8 for screen text and title fields (`OSC 0/1/2`). For example, the pair `93 FA` in the body or title can decode to the CP932 character 日, while the UTF-8 URI in a working-directory notice (`OSC 7`) and Kitty image data keep their original bytes. Title handling uses extended VT. A stream decoder can consume an ISO-2022-JP character-set switch and pass a cursor movement such as `ESC [ 2 ; 3 H` on to the terminal parser.

Transport encoding and display format are independent. With compact ASCII cells, a decoded Japanese character appears on screen as `?`. [Embedded cell formats and terminal controls](./embedded-core.md)

## Register a converter in C99

This example connects a 7-bit ASCII device. Sending the UTF-8 text `日A` produces `?A` on the wire. On receive, it substitutes `?` for any byte outside printable ASCII in screen text or titles.

```c
#include <boxrig/terminal.h>

#if BOXRIG_FEATURE_ENCODING_HOOKS
static size_t ascii_field(boxrig_encoding_channel channel, const uint8_t *input,
    size_t length, uint8_t *output)
{
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = input[i];
        if (channel == BOXRIG_ENCODING_SEND && (byte & 0xc0u) == 0x80u)
            continue; /* One output byte per UTF-8 scalar. */
        if (output) {
            bool ascii = byte < 0x80 &&
                (channel == BOXRIG_ENCODING_SEND || (byte >= 0x20 && byte != 0x7f));
            output[count] = ascii ? byte : (uint8_t)'?';
        }
        ++count;
    }
    return count;
}

static boxrig_status convert_ascii(void *context, boxrig_encoding_channel channel,
    const uint8_t *input, size_t length, bool finish,
    boxrig_encoding_buffer *buffer, size_t *written)
{
    (void)context;
    if (finish) return BOXRIG_STATUS_OK; /* ASCII has no pending shift or input. */
    size_t needed = ascii_field(channel, input, length, NULL);
    uint8_t *output;
    boxrig_status status = boxrig_encoding_reserve(buffer, needed, &output);
    if (status != BOXRIG_STATUS_OK) return status;
    *written = ascii_field(channel, input, length, output);
    return BOXRIG_STATUS_OK;
}

boxrig_status attach_ascii(boxrig_terminal *terminal)
{
    const boxrig_encoding_hooks hooks = {
        .mode = BOXRIG_ENCODING_FIELDS, .convert = convert_ascii
    };
    return boxrig_terminal_set_encoding_hooks(terminal, &hooks);
}
#endif
```

Call `attach_ascii(terminal)` after creating the terminal and before the first I/O. boxrig copies the registration. For a stateful converter, supply application-owned state in `hooks.context` and keep it alive until replacement or removal succeeds, or until the terminal is destroyed. An SDK built with `BOXRIG_FEATURE_ENCODING_HOOKS=0` omits the hook types and functions. Guard their use with `#if` as in the example. [Public API and callback signatures](../../include/boxrig/terminal.h)

## Handle fragmented input and a full output queue

Use independent states for received text (`BOXRIG_ENCODING_RECEIVE`), titles (`BOXRIG_ENCODING_TITLE`), and output (`BOXRIG_ENCODING_SEND`). Stream mode delivers titles as part of `RECEIVE`. Input may arrive one byte at a time. A CP932 decoder can accept a lead byte, retain it for the next call, and return success with zero output bytes.

Returning success from `convert` accepts the entire input fragment. Reserve the destination once with `boxrig_encoding_reserve()` and set `*written` to the number of bytes actually produced. It starts at zero. For a stateful converter, reserve the required space before advancing its state. Preserve the previous state on any error. Input and output pointers are borrowed for the duration of the callback. Handle C++ exceptions inside the callback and defer operations on the same terminal until the callback returns.

| Call result | What the application does |
| --- | --- |
| `feed()` consumes only part of the input | Advance by `consumed_out` wire bytes. Service pending output or host requests, then resubmit the remaining input |
| Send or finish returns `BOXRIG_STATUS_WOULD_BLOCK` | Get bytes with `boxrig_terminal_output_peek()`, write them to the transport, and call `boxrig_terminal_output_consume()` for the number accepted. Answer any pending host request and retry the same operation |
| A send returns `BOXRIG_STATUS_LIMIT_EXCEEDED` | A single reservation exceeds the configured limit. Split outgoing text at character boundaries or adjust `output_capacity` at creation |

For a raw receive log, record bytes once when the transport reads them. For a decoded log or a receive macro, use the optional `hooks.received` observer. It delivers bytes accepted by the parser, including control syntax, in order and once across retries. Dispatch binary receive protocols such as X/Y/ZMODEM at the transport. For sending, `boxrig_terminal_send_bytes()` queues the exact wire bytes in either conversion mode.

## Switch encodings on a live connection

1. Pause new reads and sends. Process bytes already received in the old encoding and finish any paste in progress. If the communication session has retained input, make room in the queues or answer host requests and dispatch with `BOXRIG_SESSION_READY_NONE`.
2. Call `boxrig_terminal_finish_input()`. The converter receives `finish=true`, `input=NULL`, and `length=0`. Apply the converter's incomplete-character policy. For example, it can emit `�` for a trailing CP932 lead byte. A finished channel starts fresh on its next input.
3. Call `boxrig_terminal_finish_encoding_output()` to finalize the send channel, including any final shift sequence. On `WOULD_BLOCK`, follow the table above and retry. Drain all output in the old encoding to the transport.
4. Pass the next registration to `boxrig_terminal_set_encoding_hooks()`, or pass `NULL` to resume built-in UTF-8. On `BOXRIG_STATUS_BUSY`, retain the old registration, resolve the pending work, and retry. After a successful switch, the same screen and scrollback continue in the new encoding.

Follow the same procedure to remove the hooks before saving a checkpoint. The converter state belongs to the application and an export with a registered converter returns `BOXRIG_STATUS_BUSY`. Register a fresh converter after restore and before resuming traffic. [Terminal and session handoff](./sessions.md#moving-a-live-terminal-to-another-process)

## Migrate an existing connection

If the application used `boxrig_terminal_config.encoding` and `BOXRIG_ENCODING_SHIFT_JIS`, register a converter for the connection's encoding. When moving conversion from the application connection layer into hooks, pass wire bytes directly to `feed()` and write the bytes taken from the output queue directly to the transport. Pace a paste using the number of encoded bytes written.

The C++ integration example registers elder-terms' `TerminalTextStream`. It receives 日 and a trailing lead byte in CP932, switches to UTF-8, then receives 語. The screen and decoded log contain `日�語`, and the raw log retains the received bytes. The same example checks fragmented ISO-2022-JP, a UTF-16LE query and reply, a paste sent two bytes at a time, and raw output of every byte from 0 to 255. [boxrig integration example](../../tests/test_elder_terms_encoding.cpp) and [elder-terms converter](https://github.com/kekyo/elder-terms/blob/254bddc8816b38f2e74a8baa22c971492053fcf1/elder-terms-terminal/src/terminal-sessions/terminal-text-stream.cpp)

```sh
make test-elder-terms-encoding ELDER_TERMS_DIR=../elder-terms
```

If an older SDK preparation script lists `data/encoding/whatwg/LICENSE`, remove that reference from its input list, installation manifest, and package checks. The file accompanied the retired Shift_JIS conversion table. [Example of the earlier SDK preparation script](https://github.com/kekyo/elder-terms/blob/254bddc8816b38f2e74a8baa22c971492053fcf1/scripts/prepare-boxrig.mts#L189)

[Terminal I/O settings](./terminal-io.md) · [Documentation index](./index.md)
