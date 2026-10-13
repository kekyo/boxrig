# Integrating into a Win32 application

[日本語](../../ja/platforms/win32.md)

The boxrig UI Win32 component places a terminal child window inside an existing `HWND`.
It displays text and images interpreted by boxrig core and accepts keyboard, mouse, and IME input.
Default rendering, OpenGL, and Direct3D 11 share input, allocation, and frame operations.

Your application provides the parent window and message loop.
Connect a PTY to run a shell, or use fixed text or a custom transport on its own.

## SDK and target architecture

The amd64 and i686 SDKs are built separately.
Use `make install-win32-amd64 PREFIX=...` for the standard 64-bit SDK, or the target ending in `i686` for 32-bit.
Match the SDK architecture to the application you link.

Core, UI, and PTY have different OS requirements.
A working UI does not imply that OS-supplied ConPTY is available.
See the [build guide](../building.md) for SDK creation and linking, and [Sessions](../sessions.md) for pseudoconsole connections.

## Place a component in a parent window

This function creates a terminal child window inside an existing parent.
Pass a parent window created on the same UI thread and a UI model referencing the terminal.
This is the creation excerpt; connect allocation, messages, and frame processing as described below.

```c
#include <boxrig/win32.h>

boxrig_ui_status create_terminal_child(HINSTANCE instance, HWND parent,
    boxrig_ui_model *model, boxrig_win32_terminal_component **component_out)
{
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    return boxrig_win32_terminal_component_create_child(
        instance, parent, model, &config, component_out);
}
```

After creation, allocate the component's area and make it visible.
The component owns its child window and view and borrows the parent and model.
With `create_on_target()`, which attaches to an application-owned target, the application also manages message forwarding and target lifetime.

## Process messages and render

Call `pretranslate_message()` from the message loop and check whether it handled the message.
If directly forwarded input returns `BLOCKED`, retain it and retry after capacity is restored.
Handle parent and sibling window messages normally in the application.

Advance the session, synchronize the model, and call the component's `frame()`, in that order.
When `additional_dispatch` is absent, wait for native messages, PTY activity, and any required rendering completion together.

Depending on the rendering backend, `activity_handle()` may return a handle.
Call `dispatch_activity()` only after a wait receives a notification for that handle.
It is not an API for advancing rendering without waiting for a notification.
See the [component example](../../../examples/win32/component.c) for the complete integration.

## DPI and resizing

Pass the actual client area to `allocate()` and apply confirmed grid dimensions to the terminal and PTY.
DPI and font changes also change cell dimensions, so do not continue calculating columns from old dimensions.
Only one view determines a model's grid size.

When the parent moves on screen, update the candidate window position as well.
`resynchronize_ime_placement()` reports the IME position again after movement.
It does not itself render or synchronize the model.

## Choose a rendering backend

| Purpose | API or guide |
| --- | --- |
| Default Win32 display | `create_child()` on this page |
| OpenGL child window | [OpenGL component](../../../include/boxrig/win32/opengl_component.h) and [Rendering options](../renderers.md) |
| Direct3D 11 child window | [Direct3D 11 component](../d3d11-component.md) |
| Application-owned Direct3D target | [Direct3D 11 renderer](../d3d11-renderer.md) |

You can retain the terminal and model when changing backends.
Release the old component's layout authority and rendering resources before attaching a new component.

## Transparency and parent window requirements

Giving the terminal background an alpha value and making the toplevel transparent to the desktop are separate settings.
The parent window must be configured for the rendering backend; the component does not change the parent's styles.

See [host_opacity.c](../../../examples/win32/host_opacity.c) for default rendering and the [dedicated guide](../d3d11-component.md#transparency-and-resizing) for Direct3D 11.
Retrieve the actual transparency state rather than assuming success from the requested value.

## Cleanup and deployment checks

On the UI thread that created the component, stop wait registrations and input, then destroy the component.
Afterward, release the model, terminal, and parent resources.
If a communication session exists, stop it before the terminal and PTY as well.

Check rendering APIs, the GPU, and ConPTY availability separately on the deployment target.
See the [Win32 headers](../../../include/boxrig/win32.h) and [SDK build guide](../building.md) for installed examples and public API details.

[Documentation index](../index.md)
