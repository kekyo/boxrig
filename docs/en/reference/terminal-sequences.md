# Control sequences and input protocols

[日本語](../../ja/reference/terminal-sequences.md)

Control sequences sent to a terminal specify operations such as red text, cursor movement, and screen switching.
This reference lists sequences interpreted by boxrig core and bytes returned for input and queries.

For example, the [README's red-text example](../../../README.md#add-a-terminal-display-to-an-application) uses SGR color settings.
Use [boxrig_emitter](../boxrig_emitter.md) to try short output examples, or [Terminal I/O settings](../terminal-io.md) to configure newlines and encodings through the API.

## Supported profile and notation

boxrig uses ECMA-48, DEC VT, and xterm as its basis, with the Kitty extensions listed below.
This does not mean that every feature of each specification is implemented.
For parameters absent from these tables, check the public API and tests.

| Notation | Bytes or meaning |
| --- | --- |
| ESC | `0x1b` |
| CSI | `ESC [` |
| OSC | `ESC ]` |
| DCS | `ESC P` |
| APC | `ESC _` |
| ST | `ESC \` |
| Ps, Pm | Numeric parameter, parameter list |

Spaces in sequence notation are separators.
However, SP in `CSI Ps SP q` is an actual space byte.
Control-sequence coordinates usually start at 1; C API rows and columns start at 0.
Sources are [xterm control sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html), [Kitty keyboard](https://sw.kovidgoyal.net/kitty/keyboard-protocol/), and [Kitty desktop notifications](https://sw.kovidgoyal.net/kitty/desktop-notifications/).

## Text and C0 controls

UTF-8 is interpreted incrementally by default.
Characters split across input calls are retained for the next call; invalid sequences become replacement characters.
Combining characters and emoji are retained as character clusters, with a limit of 4,096 bytes per cluster.
Ambiguous-width characters can occupy one or two columns.
See [Terminal I/O settings](../terminal-io.md) for Shift_JIS conversion and compatibility settings, and [Fonts and layout](../fonts-and-layout.md) for shaping.

| Input | Behavior and conditions |
| --- | --- |
| BEL | Report a bell as a host event |
| BS | Move the cursor back one column without erasing text |
| HT | Move to the next tab stop |
| LF, VT, FF | Newline; whether it includes CR depends on receive-newline settings and modes |
| CR | Move to the start of the line |
| SO, SI | Select the G1 or G0 character set |
| CAN, SUB | Cancel the control sequence being parsed and return to normal input |

## ESC and CSI

| Sequence | Operation | Conditions |
| --- | --- | --- |
| `ESC 7`, `ESC 8` | Save/restore cursor and attributes | Includes states such as origin and wrapping |
| `ESC D`, `ESC E`, `ESC M` | Index, next line, reverse index | Scroll when needed |
| `ESC H` | Set a tab stop | At the current column |
| `ESC ( 0`, `ESC ) 0` | Assign DEC special graphics to G0 or G1 | Selected with SO/SI |
| `ESC ( B`, `ESC ) B` | Assign ASCII to G0 or G1 | Same selection rules |
| `ESC =`, `ESC >` | Application/normal keypad mode | Affects outgoing keys |
| `ESC c` | Reset the terminal | RIS |
| `CSI Ps A/B/C/D` | Move up/down/right/left | Omitted and zero values mean 1 |
| `CSI Ps E/F/G/d` | Next line, previous line, absolute column, absolute row | Omitted and zero values mean 1 |
| `CSI row ; col H/f` | Move to a row and column | Respects origin mode and margins |
| `CSI Ps @/P/X` | Insert/delete/erase characters | Does not split wide characters |
| `CSI Ps L/M` | Insert/delete lines | Within the scrolling region |
| `CSI Ps S/T`, `CSI Ps ^` | Scroll up/down | `^` also scrolls down |
| `CSI Ps b` | Repeat the preceding character | REP |
| `CSI 0 g`, `CSI 3 g` | Clear the current/all tab stops | TBC |
| `CSI Ps J`, `CSI Ps K` | Erase the display/line | 0 erases toward the end, 1 toward the start, 2 everything; `CSI 3 J` clears history |
| `CSI ? Ps J/K` | Erase unprotected cells | Selective erase |
| `CSI top ; bottom r` | Set top/bottom margins | Invalid ranges are ignored |
| `CSI left ; right s` | Set left/right margins | When mode 69 is enabled |
| `CSI s`, `CSI u` | Save/restore the cursor | `s` when left/right margin mode is disabled |
| `CSI ! p` | Soft reset | DECSTR |
| `CSI Ps " q` | Set cell protection | 1 enables; 0/2 disables |
| `CSI 22 ; Ps t`, `CSI 23 ; Ps t` | Request title save/restore | 0 means both, 1 icon, 2 window; storage belongs to the host |

Printer controls `CSI 0/4/5 i` and memory lock/unlock are accepted as no-ops.
After reading an unknown sequence's terminator, normal text display resumes.

## Modes, screens, and scrolling

Enable or disable standard modes with `CSI Ps h/l` and DEC private modes with `CSI ? Ps h/l`.

| Mode | User-visible behavior |
| --- | --- |
| 4, 20 | Insert mode, newline mode |
| ?1, ?66 | Application cursor keys, application keypad |
| ?5, ?6, ?7 | Screen inversion, margin-relative origin, automatic wrapping |
| ?12, ?25 | Cursor blinking, cursor visibility |
| ?47, ?1047, ?1048, ?1049 | Alternate screen, switching with clearing, cursor save, save plus screen switch |
| ?69 | Left/right margins |
| ?9, ?1000, ?1002, ?1003 | X10, button, drag, and all-motion mouse tracking; mutually exclusive |
| ?1004 | Focus reporting |
| ?1006, ?1016 | SGR cell coordinates, physical-pixel coordinates; mutually exclusive |
| ?1007 | Convert the wheel to Up/Down cursor keys on the alternate screen |
| ?1034 | 8-bit Meta |
| ?2004 | Bracketed paste |
| ?2026 | Synchronized output |

1007 is disabled by default.
On the alternate screen with mouse tracking disabled, it converts one vertical wheel step into Up/Down keys, respecting DECCKM.
This conversion does not use Kitty keyboard or custom key mappings.
It survives soft resets and screen switches and is cleared by RIS.

Input parsing continues during synchronized output, while UI displays the last committed screen.
If the mode is not cleared, recovery occurs 1,000 ms after it began.
Setting it again or resizing does not extend the deadline.
Clock supply and frame processing use the same animation infrastructure as [Blinking](../blinking.md).

Primary-screen history and alternate-screen contents are independent.
Resizing rearranges text using wrapping information.
See [Retrieving, selecting, and searching text](../text.md) for APIs involving history, selection, and logical-line positions.

## SGR and the cursor

`CSI Pm m` changes attributes for subsequent text.
See [Display color filters](../color-filters.md) for rendering-side color adjustments.

| Parameter | Attribute or reset |
| --- | --- |
| 0 | Reset all attributes |
| 1, 2, 3 | Bold, faint, italic; 22 resets bold and faint, 23 resets italic |
| 4, 21, 4:1–4:5 | Single, double, and single/double/curly/dotted/dashed underlines |
| 24, 4:0 | Reset underline |
| 5, 6 | Slow/rapid blink; 25 resets both |
| 7, 8, 9 | Inverse, concealed, strikethrough; reset with 27, 28, 29 |
| 30–37, 40–47 | Basic eight foreground/background colors |
| 90–97, 100–107 | Bright eight foreground/background colors |
| 38, 48, 58 | Extended foreground/background/underline colors: indexed `5;n` or RGB `2;r;g;b`; colon separators also supported |
| 39, 49, 59 | Restore default foreground/background/underline colors |

For DECSCUSR, `CSI Ps SP q` uses 0/1 for a blinking block, 2 for a steady block, 3/4 for a blinking/steady underline, and 5/6 for a blinking/steady bar.
See [Cursor and selection](../decorations.md) for interaction with UI overrides and [Blinking](../blinking.md) for intervals.

## OSC, DCS, and APC

OSC ends with BEL or ST.
DCS, APC, PM, and SOS end with ST; an internal BEL is not a terminator.
CAN/SUB cancels a sequence in progress.
8-bit C1 DCS, APC, and ST are unsupported.

| Sequence | Supported behavior and conditions |
| --- | --- |
| OSC 0/1/2 | Request both titles / icon name / window name |
| OSC 4/104 | Set/query palette entries / reset them |
| OSC 7 | Report the working directory to the host |
| OSC 8 | Hyperlinks; retain `id` and URI across history and resizing |
| OSC 10/11/12 | Set/query default foreground/background/cursor colors |
| OSC 110/111/112 | Reset default color overrides |
| OSC 52 | Request clipboard reads and writes |
| OSC 99 | Desktop notifications, described below |
| DCS `$q` | Query a status string; see the next section |
| APC `G` | Kitty graphics; see [Graphics specification](./graphics.md) |

Colors use `#RGB` with one to four digits per component, or `rgb:R/G/B`.
Color names are unsupported.
A `?` query returns `rgb:rrrr/gggg/bbbb`.

OSC 8 links are independent of SGR, and core does not open them automatically.
The default total retained URI and ID capacity is 1 MiB; a request that cannot be registered does not enable a link.
Opening and permission belong to [Host integration](../host-integration.md).

OSC 52 accepts selections `c,p,q,s,0–7`, defaulting to `0` when omitted.
Writes are parsed as strict Base64; invalid contents become an empty write request.
`?` requests a read.
The default limit is 32 KiB; setting it to 0 disables the feature.
The host takes the request and completes it exactly once.
While a read response is pending, output ordering prevents subsequent input from overtaking it.

### OSC 99 notifications

| Field | Accepted values |
| --- | --- |
| `p` | `title`, `body`, `?`; defaults to title |
| `i` | An identifier of at most 128 alphanumeric or `_+.-` characters; `0` means no identifier |
| `d` | 0 continues chunks, 1 completes; defaults to 1 |
| `e` | 0 for UTF-8, 1 for Base64 |
| `f`, `t` | Base64 application name and notification types; at most 16 types |
| `o` | `always`, `unfocused`, `invisible` |
| `u` | Urgency 0, 1, or 2 |

Body CRLF and CR are normalized to LF; control characters other than LF and tab are rejected.
Titles, application names, and types cannot contain control characters.
An empty title uses the body as the title; if both are empty, no notification is produced.
UTF-8 chunks are limited to 2,048 bytes and Base64 chunks to 4,096 bytes.
Defaults allow 16 notifications under construction, up to 32 KiB each; setting both limits to 0 also disables queries.

The capability response is `p=title,body,?:o=always,unfocused,invisible:u=0,1,2`.
It describes terminal parsing capabilities, not OS display permission.
Closing, liveness queries, buttons, icons, action-result reporting, sounds, and timeouts are unsupported.
Unknown metadata keys are ignored.
See [Platform support](./platform-support.md) for actual notification conditions.

## Queries and responses

| Query | Response |
| --- | --- |
| `CSI 5 n` | `CSI 0 n` |
| `CSI 6 n`, `CSI ? 6 n` | Current row/column as `CSI row ; col R` or its private form |
| `CSI ? 15/25/26/75/85 n` | Respectively `?10n`, `?20n`, `?27;1;0;0n`, `?70n`, `?83n` |
| `CSI c`, `CSI > c` | `CSI ? 62 ; 6 ; 22 c`, `CSI > 1 ; 0 ; 0 c` |
| `CSI > 0 q` | DCS, `>`, a vertical bar, `boxrig`, then ST |
| `CSI Ps $ p` and private form | `CSI Ps ; Pm $ y`; Pm is 1 for enabled, 2 for disabled, 0 for unknown |
| `CSI 14 t`, `CSI 16 t` | Configured drawing-area/cell dimensions in physical pixels |
| `DCS $q selector ST` | `DCS 1$r state ST` for supported selectors, `DCS 0$r ST` otherwise |

DECRQSS selectors are `m`, `r`, `s`, `SP q`, and `"q`.
Malformed, canceled, and over-limit sequences receive no response.
If the output queue is full, retrying does not generate duplicate responses.

## Sending keys, paste, and mouse events

`send_key()` accepts a logical key, modifiers, press/repeat/release kind, and associated text.
Legacy output uses xterm sequences for F1–F20 and Kitty functional-key numbers for F21–F35, media keys, and similar keys.
Repeats use the same sequence as presses; releases are normally not sent.
Alt-modified characters receive an ESC prefix.
Control-modified ASCII maps to the corresponding C0/DEL; with Shift, the actual logical symbol takes precedence.
This key conversion does not apply to IME commits, paste, or `send_text()`.

For Kitty keyboard, query with `CSI ? u` and replace/add/remove flags with `CSI = flags ; 1/2/3 u`.
`CSI > flags u` saves and sets flags; `CSI < count u` restores them.
Primary and alternate screens have separate 16-entry stacks, discarding the oldest on overflow.
RIS resets both.

| Flag | Behavior |
| --- | --- |
| 1 | Disambiguate key sequences |
| 2 | Report repeat and release event kinds |
| 4 | Add shifted and base-layout keys |
| 8 | Report all text, functional, and modifier keys |
| 16 | With 8, append associated text; not on release |

Bracketed paste uses `CSI 200 ~` and `CSI 201 ~` delimiters.
The setting at the start remains in effect until the end, preserving the pair across mode changes.
Paste chunks containing ESC are rejected to prevent forged terminators.
Focus reporting sends `CSI I` for gaining focus and `CSI O` for losing it.

Legacy mouse reporting sends `CSI M` followed by button and coordinates; a zero-based row or column of 223 or greater returns `LIMIT_EXCEEDED`.
1006 uses 1-based decimal cell coordinates; 1016 uses 1-based physical-pixel coordinates relative to the content area.
1016 can represent negative coordinates outside the area and values beyond its dimensions.
SGR presses and motion end with `M`; releases end with `m`.

Appending one event is atomic.
On `WOULD_BLOCK`, drain the queue and retry the same event.
See [Terminal I/O settings](../terminal-io.md) for key-mapping priority and encoding conversion.

## terminfo and limits

The bundled terminal defaults to `TERM=xterm-256color`; `BOXRIG_TERM=xterm` is also available.
No custom terminfo is distributed.
Do not infer support for every xterm extension from this name alone.

CSI allows up to 32 parameters and four intermediate bytes.
An over-limit CSI is discarded through its final byte.
Control strings default to a 1 MiB limit; on overflow, input is discarded through the terminator before recovery.
Unknown DCS/APC/PM/SOS sequences, sixel, and iTerm2 images are not rendered.
See [terminal.h](../../../include/boxrig/terminal.h) for capacity and state definitions and [core tests](../../../tests/test_terminal.c) for actual acceptance conditions.

[Documentation index](../index.md)
