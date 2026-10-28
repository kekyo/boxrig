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
Save its input generation too, and discard it if the generation from `get_input_state()` changes.
Handle parent and sibling window messages normally in the application.

Advance the session, synchronize the model, and call the component's `frame()`, in that order.
When `additional_dispatch` is absent, wait for native messages, PTY activity, and any required rendering completion together.

Depending on the rendering backend, `activity_handle()` may return a handle.
Call `dispatch_activity()` only after a wait receives a notification for that handle.
It is not an API for advancing rendering without waiting for a notification.
See the [component example](../../../examples/win32/component.c) for the complete integration.

## Temporarily inhibit input

Call `boxrig_win32_terminal_component_set_input_enabled(component, false)` to inhibit input.
It cancels keys, IME commits, and clipboard paste not yet submitted to the terminal.
Selection, copy, history navigation, rendering, and application input routers remain available.

`get_input_state()` returns `ENABLED`, `STOPPING`, or `DISABLED`, plus the input generation.
If an active paste terminator cannot enter the output buffer, the disable request returns `BUSY` and enters `STOPPING`.
Retry through `frame()`, then resume with `true` once the phase is `DISABLED`.
A request made from an input callback also returns `BUSY`, preserving the old state.
Previously submitted core bytes remain queued, and terminal queries still receive replies.
The application is responsible for checking transport drain completion.

Keys held across inhibition remain suppressed through release.
Old `WM_CHAR` messages are discarded until a fresh key press after resumption; IME commits require a new composition or reconversion session.
Apply the saved input generation to application-retained native messages and directly invoked composition callbacks too.
The window's borrowed IMM context is temporarily detached, then restored on resumption or destruction.
Keep that context alive and leave its association unchanged meanwhile, following the [ImmAssociateContext lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immassociatecontext).

## Move to another parent window

Use `boxrig_win32_terminal_component_reparent(component, parent)` to move an owned child to another parent.
The standard, OpenGL, and Direct3D 11 backends share this API.
It retains the child HWND, view, selection, display settings, terminal, and communication session.

Create the parent in the same process and UI thread, with the same DPI awareness as the child.
The child itself, its descendants, and borrowed targets created with `create_on_target()` are excluded.
See the [SetParent specification](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) for native parenting and DPI-awareness constraints.

The call returns `BUSY` during held keys or mouse buttons, input capture, IME composition, paste, rendering or input callbacks, and other active work.
Finish that work and retry from the event loop.
Discard host-retained input when the generation from `get_input_state()` changes.

`OK` means the parent has been replaced, so the old parent can be destroyed.
Failure preserves the old parent.
Keep the new parent alive until the next move or component destruction.
Geometry and grid dimensions remain unchanged; pass the destination area to `allocate()`.
Request focus separately with `request_focus()`.
If the destination DPI differs, also update font metrics as described below.

The [round-trip example](../../../examples/win32/transfer.c) moves the same ConPTY session between two windows on F6.
Build it with `make build/win32/amd64/examples/win32_transfer.exe`.
Replace `amd64` with `i686` for the 32-bit version.

## DPI and resizing

Pass the actual client area to `allocate()` and apply confirmed grid dimensions to the terminal and PTY.
DPI and font changes also change cell dimensions, so do not continue calculating columns from old dimensions.
When output DPI changes, read the policy with `get_font_rendering_policy()`, update `dpi_x` and `dpi_y`, and pass it to `set_font_rendering_policy()`.
Then pass the new area to `allocate()` and complete layout actions normally.
The move API does not query DPI and preserves the font policy.
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
