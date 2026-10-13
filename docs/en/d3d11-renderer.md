# Rendering to an application-owned Direct3D 11 target

[日本語](../ja/d3d11-renderer.md)

You can draw text and images laid out by boxrig UI into a Direct3D 11 texture owned by your application.
Use this to combine a terminal with other rendering or generate terminal images offscreen.
The [complete example](../../examples/win32/d3d11_renderer.c) can read rendered pixels back and save them to `boxrig-d3d11.ppm`.

Your application manages the device and texture, plus the swapchain and Present when displaying on screen.
boxrig core manages terminal contents, while boxrig UI and this renderer handle text layout and drawing.

## When to use a component instead

Use the [Direct3D 11 component](./d3d11-component.md) to place a terminal in a parent window and receive input.
With a standalone renderer, your application connects view creation, layout, input, and model synchronization.

Create the SDK with `make install-render-d3d11-amd64 PREFIX=...` or the i686 target.
The pkg-config name is `boxrig-render-d3d11`.
The installed `share/boxrig/examples/win32/d3d11_renderer.c` is the complete example.

## Supply a device and target

Fill in the configuration's `device` and `target` and pass it to `boxrig_d3d11_renderer_create()`.
The device must support BGRA interoperability, and the target must be a resource on that same device.
See also the [Direct2D surface requirements](https://learn.microsoft.com/en-us/windows/win32/api/d2d1_1/nf-d2d1_1-id2d1devicecontext-createbitmapfromdxgisurface%28idxgisurface_constd2d1_bitmap_properties1_id2d1bitmap1%29).

| Item | Accepted configuration |
| --- | --- |
| Resource | Either `texture` or `surface` |
| Structure | A normal render-target resource with one mip, one array element, and sample count 1 |
| Color format | `B8G8R8A8_UNORM` or `B8G8R8A8_UNORM_SRGB` |
| Alpha | `PREMULTIPLIED` or `IGNORE` |
| Dimensions and DPI | Physical-pixel dimensions matching the resource, and positive DPI |
| Generations | Nonzero generations for both device and target |

A CPU-readback resource or a surface from another device cannot be used directly as the target.
The application performs readback using a separate resource.

## Draw and accept a frame

This function draws the terminal into a configured texture and accepts those pixels for the application.
Pass an existing renderer and an allocated and synchronized UI view.
The excerpt does not include device or texture creation, or window presentation.

```c
#include <boxrig/render/d3d11.h>

boxrig_renderer_status render_terminal(boxrig_d3d11_renderer *renderer,
    boxrig_ui_view *view, uint64_t device_generation,
    uint64_t target_generation)
{
    boxrig_renderer_frame_token token;
    boxrig_renderer_status status = boxrig_d3d11_renderer_prepare_frame(
        renderer, view, NULL, device_generation, target_generation, &token);
    if (status != BOXRIG_RENDERER_OK) return status;

    /* Keep this target's pixels as the completed frame. */
    return boxrig_d3d11_renderer_accept_frame(renderer, &token);
}
```

After prepare succeeds, regions requiring redraw are still retained.
Call abort instead of accept if you do not use the result.
Duplicate acceptance and tokens from old generations are rejected.
Exclude other access to the same target from drawing through completion.

By default, call only from the creating thread.
Even the multithread configuration does not allow concurrent calls.
The renderer retains COM references, but destroy it before releasing the application's final device or target reference.

## Fonts and images

Font selection and text layout use the common DirectWrite implementation.
Register file or memory fonts, then pass their resource IDs in font specifications.
Keep file fonts available throughout the renderer's lifetime; memory registration copies its input data.

Match DPI between the target and text-rendering settings.
For transparent surfaces, choose an available mode such as grayscale instead of ClearType.
Color glyphs using COLR/CPAL layers depend on DirectWrite 2 availability and policy; check `color_layers`.
Do not treat SVG and bitmap color glyphs as the same capability.

Image counts, image bytes, and cell counts have limits.
If a single image exceeds a limit, the frame fails and leaves the redraw request pending.
Use `get_statistics()` and `get_diagnostic()` to inspect usage, redraw work, and effective capabilities.

## Target changes and device loss

For a size change, pass a new resource and an increased target generation to `set_target()`.
A failed ordinary change preserves the previous target.
You cannot change targets while a frame is in progress.
Keep DPI unchanged when switching targets on the same renderer; recreate it with new settings if DPI changes.

If diagnostics report TARGET loss, prepare a new target.
For DEVICE loss, the application recreates the device and renderer.
The renderer does not reset the application's device itself.
You can retain the original HRESULT and `device_reason` for diagnostics.

## Runnable example and requirements

The [complete example](../../examples/win32/d3d11_renderer.c) reads target pixels back and saves them to `boxrig-d3d11.ppm`.
`--check` skips file output and verifies pixels and frame acceptance.
Since the example creates its own device, add `-ld3d11` to the SDK link options.

The distinction between OS requirements and verified environments is the same as in the [component environment requirements](./d3d11-component.md#environment-and-sdk).
If a required Direct2D interface cannot be created, creation fails with an HRESULT and leaves the output NULL.

[Public header](../../include/boxrig/render/d3d11.h), [Rendering options](./renderers.md), [Documentation index](./index.md)
