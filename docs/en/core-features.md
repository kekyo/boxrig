# Choose core features and compare binary size and memory use

[日本語](../ja/core-features.md)

Select the boxrig core features your application uses at compile time. For example, a desktop terminal can use Unicode text and the alternate screen while leaving out image handling.
When choosing a configuration, compare the code and read-only data in the linked executable as well as the memory allocated for each terminal.

## Choose features for an application

A cell holds a character and its display attributes. The peer sends VT control sequences to position the cursor, switch screens, and perform other terminal operations.

| Application | Build setting | Effect |
| --- | --- | --- |
| Interactive terminal | Keep the default Unicode cells and extended VT controls | Full-screen applications can use the alternate screen, and images and search are available |
| Text-only desktop terminal | `BOXRIG_FEATURE_KITTY_GRAPHICS=0` | Keep Unicode and the alternate screen and omit Kitty image handling and libpng from the linked binary |
| Command-line tool that searches terminal output without a display | `BOXRIG_FEATURE_TEXT_MONITOR=0` and `BOXRIG_FEATURE_TEXT_RECOGNIZER=0` | Keep ordinary text search and omit the buffers for monitoring and regex-based recognition |
| Log viewer for colorized command output | `BOXRIG_FEATURE_EXTENDED_VT=0` | Keep Unicode cells and limit the controls to those needed for logs, including color and line erase |
| Log display that needs only ASCII and 16 colors | `BOXRIG_FEATURE_COMPACT_CELLS=1` | Use two bytes per cell on a single screen with zero scrollback |

Unicode cells retain character clusters and their column widths. With the defaults, for example, `e` followed by a combining acute accent (U+0301) occupies one column, and the Hangul syllable `가` (U+AC00) occupies two. The renderer draws glyphs from the available fonts. [Character widths and fonts](./fonts-and-layout.md)

A compact cell uses one byte for an ASCII character and four bits each for the foreground and background. Each unrepresentable Unicode scalar appears as a one-column `?`.
The transport encoding is independent of the cell format. The built-in UTF-8 decoder accepts multilingual text. Register a converter for a connection that uses another encoding. [Connect a character encoding converter](./encoding-hooks.md)

## Build an SDK with selected features

To build a static SDK for a terminal that displays text without images, run the following command from the source root. [Build prerequisites](./building.md)

```sh
make install-core BUILD_DIR=build/text-terminal \
  CPPFLAGS='-DBOXRIG_FEATURE_KITTY_GRAPHICS=0' PREFIX="$PWD/build/text-sdk"
```

The static library and headers are installed in `build/text-sdk`. To build the archive alone, run `make core` with the same `BUILD_DIR` and `CPPFLAGS`.

The source `<boxrig/config.h>` provides `#ifndef` defaults. Use the literal integers `0` and `1` in `CPPFLAGS`. The installed header records the resolved values. Compile consumers with that header and the library from the same SDK. Test the value with `#if BOXRIG_FEATURE_ENCODING_HOOKS`, for example. A conflicting application definition produces a compile-time error. [Configuration header](../../include/boxrig/config.h)

## Feature definitions and defaults

Prefix each name below with `BOXRIG_FEATURE_`. The ASCII/16-color column gives the defaults when you select `COMPACT_CELLS=1`.

| Definition | Unicode cells | ASCII/16-color cells | What `1` enables |
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

For example, the following command builds a core with Unicode text and column widths, external encoding hooks, and controls limited to log output. The next section includes measurements for this configuration.

```sh
flags='-DBOXRIG_FEATURE_EXTENDED_VT=0 -DBOXRIG_FEATURE_KITTY_GRAPHICS=0'
flags="$flags -DBOXRIG_FEATURE_TEXT_MONITOR=0 -DBOXRIG_FEATURE_TEXT_RECOGNIZER=0"
flags="$flags -DBOXRIG_FEATURE_HISTORY_DEFLATE=0 -DBOXRIG_FEATURE_CHECKPOINT=0"
make core BUILD_DIR=build/unicode-log CPPFLAGS="$flags -DBOXRIG_FEATURE_ALT_SCREEN=0"
```

A compact build omits utf8proc, libpng, and zlib. Compact cells accept `LOG_CURSOR` and `ENCODING_HOOKS`. Explicitly enabling extended VT or any of the six terminal services with compact cells produces a configuration error. The compact public API covers terminal creation, resize, receiving bytes, reading cells and the cursor, and sending text and raw bytes. Use Unicode cells for the standard UI or key and paste events. [Public terminal API](../../include/boxrig/terminal.h)

## Compare code size and memory use

These measurements use Linux x86-64, GCC 13.3.0, `-O2`, and the same statically linked C log driver. Each terminal has 80 columns, 24 rows, and zero scrollback. Code and read-only data are section sizes in the linked ELF. Heap figures count live requested bytes immediately after terminal creation.

| Configuration | Code (`.text`) | Constants (`.rodata`) | Initial heap |
| --- | ---: | ---: | ---: |
| Default Unicode terminal | 435,168 B | 385,492 B | 5,287,962 B |
| Unicode terminal without images, monitoring, recognition, compressed history, checkpoints, or alternate screen | 131,646 B | 348,724 B | 1,226,544 B |
| Same Unicode terminal with controls limited to log output | 123,278 B | 348,340 B | 160,920 B |
| ASCII log with encoding hooks | 15,167 B | 872 B | 4,848 B |
| ASCII log without encoding hooks | 11,614 B | 872 B | 4,768 B |

Grid dimensions also affect heap use. At 40 columns by 15 rows, the 600 ASCII cells take 1,200 bytes. The terminal's initial heap was 1,984 bytes without hooks and 2,064 bytes with hooks. Each open terminal needs memory for its own screen and retained history. When sizing the whole application, also budget for peak I/O, the converter, fonts, drawing buffers, and allocator overhead. Run `make test-embedded-log` to repeat the measurement under the same conditions. [Measurement driver](../../tests/measure_embedded_core.c)

## Control sequences for logs

With `EXTENDED_VT=0`, core recognizes carriage return (CR), line feed (LF), backspace (BS), and horizontal tab (HT). Text wraps at the right edge and scrolls at the bottom. In the table, `CSI` stands for an ESC byte (`0x1b`) followed by `[`.

| Operation | Sequence |
| --- | --- |
| Foreground and background colors | `CSI … m`. Foreground `30–37` and `90–97`, background `40–47` and `100–107`. `39` and `49` restore the respective default colors, and `0` restores both |
| Line erase | `CSI 0 K` from cursor to end of line, `CSI 1 K` from start of line through the cursor, `CSI 2 K` for the whole line |
| Positioning with `LOG_CURSOR=1` | `CSI n A/B/C/D` for relative movement, `CSI n G` for a column, `CSI row ; column H/f` for a position. Rows and columns start at 1 |
| Visibility and erase with `LOG_CURSOR=1` | `CSI ? 25 h/l` shows/hides the cursor. `CSI 0/1/2 J` erases to the end of the screen, from the start through the cursor, or the entire screen |

For example, `"\r\033[32mOK\033[K"` writes a green `OK` at the start of the line and erases any old text to its right. Log displays skip title commands (OSC) and device control strings (DCS) through their terminator. Unicode builds select Kitty image handling independently with `KITTY_GRAPHICS`. [Default terminal control sequences](./reference/terminal-sequences.md)

## Display an ASCII log in 16 colors

This example displays a serial log containing alphanumeric text received as UTF-8. It uses an SDK with compact ASCII cells and the encoding hooks disabled.

```sh
sdk="$PWD/build/serial-sdk"
flags='-DBOXRIG_FEATURE_COMPACT_CELLS=1 -DBOXRIG_FEATURE_ENCODING_HOOKS=0'
make install-core BUILD_DIR=build/serial CPPFLAGS="$flags" PREFIX="$sdk"
cc -std=c99 -I"$sdk/include" tests/test_embedded_log.c \
  "$sdk/lib/libboxrig.a" -lm -o build/serial/log-demo
build/serial/log-demo
```

The last command feeds a log to the terminal and prints `PASS` after checking wrapping, scrolling, colors, line erase, and changed regions. [C log driver](../../tests/test_embedded_log.c)

### Paint the changed cells

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

API rows and columns start at zero. In compact cells `cell.text[0]` is the ASCII character, and `foreground.index` and `background.index` are color indices from 0 to 15. Repaint the background of erased spaces too. Cell text is borrowed until the next terminal mutation.

Resizing a grid that uses compact cells preserves the intersecting cells at the top left and marks the entire screen for repainting. Use `boxrig_terminal_invalidate()` to request a full redraw and `boxrig_terminal_get_cursor()` to obtain the position and visibility when overlaying the cursor. For example, an application using LovyanGFX or LVGL on an embedded display would implement `paint` with its chosen drawing library. [LovyanGFX](https://github.com/lovyan03/LovyanGFX/) and [LVGL](https://github.com/lvgl/lvgl/).

[Documentation index](./index.md)
