# Choosing a rendering backend

[日本語](../ja/renderers.md)

boxrig UI can display a terminal inside an existing window or render it to an application-owned GPU target.
Choose a platform component for window placement, or a renderer for output to an FBO or texture owned by your application.
Both configurations share boxrig core, which holds the terminal's characters and attributes.

You can also retrieve cells from core and implement all rendering yourself.
See the [README](../../README.md#interpret-terminal-output-without-a-display) for a minimal example and the [core step-by-step guide](./step-by-step/step-by-step-core.md) for implementation instructions.

## Standard components and rendering APIs

A component combines input, allocation, fonts, the render target, and presentation completion.
Start by choosing [X11](./platforms/x11.md), [GTK](./platforms/gtk.md), [Wayland](./platforms/wayland.md), or [Win32](./platforms/win32.md).

When using a renderer alone, your application manages view creation, allocation, frame acceptance, native presentation, and resource lifetimes.
Received data and model synchronization work the same way in both cases.

## Choose a rendering backend

| Backend | Main purpose | What the application provides |
| --- | --- | --- |
| Pango/Cairo | Standard Linux components | Parent window, GTK container, parent surface, etc. |
| DirectWrite/GDI | Standard Win32 component | Parent window |
| OpenGL component | GPU display on X11, Wayland, and Win32 | A compatible parent and event loop |
| GTK OpenGL configuration | GPU display in a GTK widget | GTK allocation and frame scheduling |
| OpenGL renderer | Drawing to an application FBO or similar target | Context, function resolution, target, and its generation |
| Direct3D 11 component | Win32 GPU display and transparency | A parent window meeting the requirements |
| Direct3D 11 renderer | Drawing to an application texture | Device and render target |

Core retains the terminal's characters and attributes for every backend.
Choosing a renderer does not change the terminal protocol.
Standard components remain available in builds that do not link optional rendering libraries.

## Prepare an OpenGL target

The [OpenGL public API](../../include/boxrig/render/opengl.h) targets OpenGL 3.3 core or OpenGL ES 3.0.
The application supplies a current context, a function resolver callback, and context and thread identities.
The library does not create a context or make one current on its own.

Targets may use RGBA8, sRGB8 alpha8, or opaque RGB8 storage.
Specify the alpha interpretation, physical-pixel dimensions, and a monotonically increasing generation.
RGB8 has no alpha channel and requires `BOXRIG_GL_ALPHA_OPAQUE`.
RGB8 requires a nonzero framebuffer whose lack of alpha storage can be verified.
The current target contract does not accept multisampling.
See [runtime.h](../../include/boxrig/render/runtime.h) for details.

Start with the [runnable FBO example](../../examples/opengl_renderer/main.c) to follow context creation through pixel verification.
Install the SDK with `make install-render-ogl PREFIX=...`; its pkg-config name is `boxrig-render-ogl`.

## Frame completion and redraw

This function draws a terminal to an application-owned FBO and accepts the rendering result.
Prepare a renderer, its matching lifecycle, and an allocated and synchronized UI view before calling it.
This excerpt completes a frame when the FBO pixels are accepted; it does not present a window.

```c
#include <boxrig/ui/renderer_lifecycle.h>

boxrig_renderer_status draw_frame(boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view, uint64_t target_generation)
{
    boxrig_renderer_frame_token token;
    boxrig_renderer_status status = boxrig_renderer_lifecycle_prepare_frame(
        lifecycle, view, NULL, target_generation, &token);
    if (status != BOXRIG_RENDERER_OK) return status;

    /* Accept the off-screen result without presenting a window. */
    return boxrig_renderer_lifecycle_accept_frame(lifecycle, &token);
}
```

For window presentation, determine whether presentation succeeded after prepare, then call accept on success or abort to cancel.
Regions requiring redraw remain pending until accept.
Keep the view and rendering resources alive from prepare through accept or abort.

A standalone renderer does not swap, read pixels back, or block for completion.
If a component handles presentation and waiting, follow that component's activity and event contracts.

## Recreate a target

When dimensions change, prepare a new target and supply a new generation.
Do not accept a frame from an old generation into a new target.
Use return values and diagnostics to check the state after a failed change.

If the context itself is lost, recreate resources and the lifecycle instead of continuing to render with old identities.
The model and terminal can outlive the render target, allowing you to rebuild the display without losing communication contents.

## Direct3D 11 and custom implementations

The [Direct3D 11 component](./d3d11-component.md) includes window presentation; the [Direct3D 11 renderer](./d3d11-renderer.md) renders to application-owned targets.

For a custom backend, consult the public contracts for [views](../../include/boxrig/ui/view.h) and [lifecycles](../../include/boxrig/ui/renderer_lifecycle.h).
Besides reading terminal state, you must handle redraw regions, text layout, selection and IME overlays, and frame acceptance.

[Documentation index](./index.md)
