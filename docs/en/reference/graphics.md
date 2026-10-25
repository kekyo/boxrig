# Images inside the terminal

[日本語](../../ja/reference/graphics.md)

Kitty graphics can display still images and animations on the same terminal screen as text.
boxrig core interprets image data and placements, and boxrig UI composites them with text.
The screenshot below shows `boxrig_emitter`'s `kitty-image-static` case in `boxrig_term`.

![Kitty graphics images inside the terminal](../../../images/kitty-image-static.png)

See [boxrig_emitter](../boxrig_emitter.md#choose-a-case) to try the same display.
This document describes sender-side parameters and their supported scope.
See [Host integration](../host-integration.md) for permissions and asynchronous handling of external files, and [Background settings](../background.md) for images fixed to the view.

## Supported image protocol

boxrig interprets `APC G header ; payload ST` from the [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/).
It supports still images, chunked transfers, external data, normal and relative placements, Unicode placeholders, and animation.
sixel and iTerm2 images are unsupported.

Text backgrounds, images, text, and the cursor are composited in layers.
An image with negative `z` is below text; `z` of 0 or greater is above text.
A sufficiently small negative `z` places it below cell backgrounds.
The cursor and IME preedit are drawn above these layers.

## Register and place images

| Operation | Parameter | Result and conditions |
| --- | --- | --- |
| Register | `a=t` | Retain the image; also the default when omitted |
| Register and place | `a=T` | Place at the cursor position when reception completes |
| Query | `a=q` | Validate the transfer without retaining the image |
| Place | `a=p` | Reference a registered image |
| Delete | `a=d` | Delete placements or images under the conditions below |

Registering a nonzero image ID again replaces the old image and its placements after successful decoding.
An ID of 0 or an omitted ID permits multiple anonymous images, but subsequent `a=p` commands cannot reference them.
Registration by image number allocates a new ID; queries and deletion for that number apply to the most recently registered live image.
The 32-bit ID on the wire differs from the 64-bit resource identifier returned by the API.

Placements support source cropping with `x/y/w/h`, within-cell pixel offsets `X/Y`, display columns and rows `c/r`, stacking order `z`, a placement ID, and cursor-movement settings.
Placement returns `ENOTSUP` unless the host has set positive cell dimensions with `boxrig_terminal_set_cell_pixel_geometry()`.
After font changes, pixel positions are resolved again while preserving logical placement parameters.

For virtual placements with `U=1`, the U+10EEEE placeholder's foreground encodes the image ID, its underline color encodes the placement ID, and protocol-defined combining characters encode rows, columns, and upper ID bits.
This image placeholder occupies one cell regardless of the ambiguous-width setting or font.
`P/Q/H/V` specify placement relative to a parent placement.
A missing parent produces `ENOPARENT`, a cycle `ECYCLE`, and a hierarchy deeper than 16 levels `ETOODEEP`.

## Update and delete

Lowercase deletion removes placements; uppercase deletion also releases image data that is no longer referenced.

| `d` value | Target |
| --- | --- |
| `a/A` | Placements intersecting the viewport |
| `i/I`, `n/N` | Image ID or image number; add `p` to specify a placement ID |
| `r/R` | Image IDs from `x` through `y` |
| `c/C` | Placements intersecting the current cursor cell |
| `p/P`, `q/Q` | Placements intersecting the 1-based `x/y` cell; `q/Q` also requires matching `z` |
| `x/X`, `y/Y`, `z/Z` | The specified column, row, or stacking order |

### Animation

| Operation | Main parameters and conditions |
| --- | --- |
| Add or edit a frame, `a=f` | `r=0` or omitted adds a frame; 1 or greater edits one. The initial image is frame 1 |
| Composite, `a=c` | Composite a rectangle from frame `r` into frame `c`; `C=0` uses source-over and 1 replaces |
| Playback control, `a=a` | `c` selects the displayed frame, `r/z` its duration, `s` the state, and `v` repetition |
| Delete a frame, `a=d,d=f/F` | `r` selects the frame; 0 means the first, and values beyond the end mean the last |

Every frame has the initial image's canvas dimensions.
When adding, `z` defaults to 40 ms; a negative value means no delay.
When editing, 0 preserves the existing duration.
`X=0` composites and 1 replaces.
Composition is rejected if its rectangle extends outside the image or regions of the same resource overlap.

For playback, `s=1` stops, 2 means loading, and 3 loops.
`v=1` repeats indefinitely, values greater than 1 specify `v-1` cycles, and 0 preserves the current value.
A loop with no positive-duration frame is not advanced.
Deletion renumbers subsequent frames; deleting the displayed frame selects the next, or the previous if no next frame exists.
Lowercase deletion of the last frame retains the image; uppercase deletion removes the image itself.

Playback requires a monotonic clock.
After a long pause, it advances to the state corresponding to elapsed time without drawing every intermediate frame.
Clock and redraw integration is shared with [Blinking](../blinking.md).

## Transfers and host permissions

| `t` | Transfer method | Conditions |
| --- | --- | --- |
| `d` | Direct Base64 transfer | Default; `m=1` continues and the final chunk uses `m=0` |
| `f` | Regular file | Requires host permission to read |
| `t` | Temporary file | The host handles reading and deletion |
| `s` | Shared memory | The host resolves POSIX shared memory, Windows named mappings, or an equivalent |

Base64 rejects whitespace, URL-safe characters, and invalid padding.
Direct continuation chunks use multiples of four characters, and subsequent headers specify only `m/q`.
Frame transfers can also use `a=f`.
An intervening different command cancels the transfer.
The position for `a=T` is determined when the final ST is received.

External names are also sent in Base64 and must not contain NUL.
`O/S` specifies the read range; `S=0` reads to the end within the capacity limit.
External transfers cannot use chunk continuation `m=1`.
Core passes requests to the host instead of opening files or shared memory directly.

The bundled terminal rejects external transfers by default and allows them with `BOXRIG_ALLOW_GRAPHICS_EXTERNAL=1`.
Temporary files must be under an allowed temporary directory and have `tty-graphics-protocol` in their name.
Deletion is attempted whether reading succeeds or fails.
Embedding applications implement permission and completion handling suitable for their own use.

## Formats, dimensions, and capacity limits

Formats are RGB with `f=24`, RGBA with `f=32`, and PNG with `f=100`.
`o=z` is zlib-format compression; raw deflate and gzip are not autodetected.
Internally, images are stored as straight-alpha RGBA8 in sRGB.
PNG palettes, grayscale, 16-bit data, interlacing, and tRNS are interpreted, but additional APNG frames are not played as animation.

| Limit or setting | Default or fixed maximum |
| --- | --- |
| Header / payload in one sequence | 1,024 bytes / 4,096 bytes |
| Image width and height | At most 1,000,000 pixels each; capacity limits also apply |
| `graphics_storage_capacity` | 128 MiB |
| `graphics_decode_capacity` | 192 MiB |
| `graphics_image_capacity` | 4,096 images |
| `graphics_placement_capacity` | 8,192 placements |
| `graphics_transport_name_capacity` | 4 KiB |
| `graphics_animation_storage_capacity` | 640 MiB |
| `graphics_frame_capacity` | 16,384 frames |

Memory for the full limits is not allocated at creation.
Allocation happens as needed, and unused images and other data are released according to retained usage.
A failed new transfer preserves previously valid images.

Setting all seven capacity fields to 0 disables graphics.
The four basic fields cannot be only partially zero.
Setting only external-name capacity to 0 disables external transfers; setting both animation fields to 0 disables animation.
See [terminal.h](../../../include/boxrig/terminal.h) for exact configuration conditions.

## Unsupported parameters and responses

Responses use `APC G`, followed by an image ID or related identifiers and `OK` or an error name.
Main errors include `EINVAL`, `EBADZLIB`, `EBADPNG`, `ENOENT`, `ENOSPC`, `ENOTSUP`, `EACCES`, and `EIO`.
`q=0` enables all responses, 1 only errors, and 2 no responses.
No response is sent without an image ID or number.

Responses append up to 160 bytes atomically.
A full output queue causes `WOULD_BLOCK` at the final ST; retrying does not duplicate registration or the response.

With OS-supplied ConPTY, image sequences can be lost during VT interpretation and re-emission before reaching boxrig.
See the [upstream discussion](https://github.com/microsoft/terminal/issues/1985) and [Platform support](./platform-support.md).
Core's ability to parse a sequence and a child process's ability to deliver it unchanged are separate conditions.

[Documentation index](../index.md)
