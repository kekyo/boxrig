# Display a serial log on a small screen

[日本語](../ja/embedded-core.md)

For a small display attached to a serial device, you can build boxrig core with the characters and control sequences the device uses. Feed the received bytes to core, and it tracks wrapping, scrolling, colors, and the regions that need repainting. The application supplies the font and draws the pixels.

## Choose the characters and controls

| What you want to display | Build setting | Example |
| --- | --- | --- |
| Terminal applications with Japanese text | Keep the default Unicode cells and extended VT controls | A full-screen application can use the alternate screen |
| Instrument logs with Japanese text | Keep Unicode cells and set `BOXRIG_FEATURE_EXTENDED_VT=0` | In `日Z` the first character occupies two columns and `Z` starts in the third |
| An alphanumeric serial log in 16 colors | Set `BOXRIG_FEATURE_COMPACT_CELLS=1` | The same input appears as `?Z` in two columns |

A cell holds a character and its display attributes. A compact cell uses one byte for an ASCII character and four bits each for the foreground and background. Each unrepresentable Unicode scalar appears as a one-column `?`. Choose the transport encoding separately. For example, to display Japanese from a CP932 device, use Unicode cells and have an application converter decode the incoming text to UTF-8.

## Build a compact static SDK

The following commands build and install core for an alphanumeric UTF-8 log with encoding hooks disabled. Run them from the source root. The build guide covers the prerequisites for generating the SDK. [Build prerequisites](./building.md)

```sh
sdk="$PWD/build/serial-sdk"
flags='-DBOXRIG_FEATURE_COMPACT_CELLS=1 -DBOXRIG_FEATURE_ENCODING_HOOKS=0'
make install-core BUILD_DIR=build/serial CPPFLAGS="$flags" PREFIX="$sdk"
cc -std=c99 -I"$sdk/include" tests/test_embedded_log.c \
  "$sdk/lib/libboxrig.a" -lm -o build/serial/log-demo
build/serial/log-demo
```

The last command feeds a log to the terminal and prints `PASS` after checking wrapping, scrolling, colors, line erase, and changed regions. To build just the archive, run `make core` with the same `BUILD_DIR` and `CPPFLAGS`. [C log driver](../../tests/test_embedded_log.c)

The source `<boxrig/config.h>` provides `#ifndef` defaults. Use the literal integers `0` and `1` in `CPPFLAGS`. The installed header records the resolved values. Compile consumers with that header and the library from the same SDK. Test the value with `#if BOXRIG_FEATURE_ENCODING_HOOKS`, for example. A conflicting application definition produces a compile-time error. [Configuration header](../../include/boxrig/config.h)

## Feature definitions and defaults

Prefix each name below with `BOXRIG_FEATURE_`. The compact column gives the settings when you select `COMPACT_CELLS=1`.

| Definition | Default | Compact | What `1` enables |
| --- | ---: | ---: | --- |
| `COMPACT_CELLS` | 0 | 1 | One ASCII screen with 16 colors and zero scrollback |
| `EXTENDED_VT` | 1 | 0 | Extended VT controls and host requests such as title changes |
| `LOG_CURSOR` | 0 | 0 | Cursor movement, visibility, and display erase for log displays. Extended VT already includes these controls |
| `ENCODING_HOOKS` | 1 | 1 | An application-provided encoding converter per terminal |
| `KITTY_GRAPHICS` | 1 | 0 | Kitty image transfers and placement, using libpng and zlib |
| `TEXT_MONITOR` | 1 | 0 | Monitoring for occurrences of specified text |
| `TEXT_RECOGNIZER` | 1 | 0 | Regular-expression recognition of terminal text |
| `HISTORY_DEFLATE` | 1 | 0 | Scrollback compression using zlib |
| `CHECKPOINT` | 1 | 0 | Saving and restoring terminal state using a checkpoint and zlib |
| `ALT_SCREEN` | 1 | 0 | A separate alternate screen for full-screen applications |

Unicode builds let you select these services independently. With `HISTORY_DEFLATE=0` you can keep uncompressed history by setting `scrollback_capacity` to the desired row count at creation. Ordinary text search remains available when monitoring and recognition are off. Disabling images, history compression, and checkpoints together removes the zlib dependency.

Compact cells accept `LOG_CURSOR` and `ENCODING_HOOKS`. Explicitly enabling extended VT or any of the six terminal services with compact cells produces a configuration error. The compact public API covers terminal creation, resize, receiving bytes, reading cells and the cursor, and sending text and raw bytes. Use Unicode cells for the standard UI or key and paste events. [Public terminal API](../../include/boxrig/terminal.h)

## Control sequences for logs

With `EXTENDED_VT=0`, core recognizes carriage return (CR), line feed (LF), backspace (BS), and horizontal tab (HT). Text wraps at the right edge and scrolls at the bottom. In the table, `CSI` stands for an ESC byte (`0x1b`) followed by `[`.

| Operation | Sequence |
| --- | --- |
| Foreground and background colors | `CSI … m`. Foreground `30–37` and `90–97`, background `40–47` and `100–107`. `39` and `49` restore the respective default colors, and `0` restores both |
| Line erase | `CSI 0 K` from cursor to end of line, `CSI 1 K` from start of line through the cursor, `CSI 2 K` for the whole line |
| Positioning with `LOG_CURSOR=1` | `CSI n A/B/C/D` for relative movement, `CSI n G` for a column, `CSI row ; column H/f` for a position. Rows and columns start at 1 |
| Visibility and erase with `LOG_CURSOR=1` | `CSI ? 25 h/l` shows/hides the cursor. `CSI 0/1/2 J` erases to the end of the screen, from the start through the cursor, or the entire screen |

For example, `"\r\033[32mOK\033[K"` writes a green `OK` at the start of the line and erases any old text to its right. Log displays skip title commands (OSC) and device control strings (DCS) through their terminator. Unicode builds select Kitty image handling independently with `KITTY_GRAPHICS`. [Default terminal control sequences](./reference/terminal-sequences.md)

## Paint the changed cells

Start with `boxrig_terminal_config_default()` and set `columns` and `rows` before creating the terminal. Keep `scrollback_capacity` at its compact default of `0`. Feed input with `boxrig_terminal_feed()` and check the consumed byte count. The following function repaints the changed regions. Supply a `paint` function that draws a cell's text and background at the given position.

```c
#include <boxrig/terminal.h>

void draw_log(boxrig_terminal *terminal,
    void (*paint)(size_t row, size_t column, const boxrig_cell_view *cell))
{
    boxrig_dirty dirty;
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        for (size_t i = 0; i < dirty.column_count; ++i) {
            size_t column = dirty.column + i;
            boxrig_cell_view cell;
            if (boxrig_terminal_get_cell(terminal, dirty.row, column, &cell) == BOXRIG_STATUS_OK)
                paint(dirty.row, column, &cell);
        }
    }
}
```

API rows and columns start at zero. In compact cells `cell.text[0]` is the ASCII character, and `foreground.index` and `background.index` are color indices from 0 to 15. Repaint the background of erased spaces too. Cell text is borrowed until the next terminal mutation. Use `boxrig_terminal_invalidate()` to request a full redraw and `boxrig_terminal_get_cursor()` to overlay the cursor. A LovyanGFX or LVGL port would supply the equivalent of `paint` for its display.

## Estimate flash and RAM

These measurements use Linux x86-64, GCC 13.3.0, `-O2`, and the same statically linked C log driver. Each terminal has 80 columns, 24 rows, and zero scrollback. Code and read-only data are section sizes in the linked ELF. Heap figures count live requested bytes immediately after terminal creation.

| Configuration | Code (`.text`) | Constants (`.rodata`) | Initial heap |
| --- | ---: | ---: | ---: |
| Default Unicode terminal | 435,168 B | 385,492 B | 5,287,962 B |
| Unicode log without images, monitoring, recognition, compressed history, checkpoints, or alternate screen | 123,278 B | 348,340 B | 160,920 B |
| ASCII log with encoding hooks | 15,167 B | 872 B | 4,848 B |
| ASCII log without encoding hooks | 11,614 B | 872 B | 4,768 B |

At 40 columns by 15 rows, the 600 compact cells take 1,200 bytes. The terminal's initial heap was 1,984 bytes without hooks and 2,064 bytes with hooks. Budget separately for peak I/O, the application's converter, fonts, drawing buffers, and allocator overhead on the target. Run `make test-embedded-log` to reproduce the host measurement. [Measurement driver](../../tests/measure_embedded_core.c)

[Documentation index](./index.md)
