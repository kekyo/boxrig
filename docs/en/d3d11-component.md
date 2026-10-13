# Using a Direct3D 11 child window

[日本語](../ja/d3d11-component.md)

The boxrig UI Direct3D 11 component places a GPU-rendered terminal inside an existing Win32 window.
Its terminal, model, input, and allocation APIs are shared with the standard Win32 component.
Your application supplies the parent window, while the component manages the device, swapchain, and presentation-completion waits.

The [runnable example](#runnable-example) demonstrates fixed text, transparency, and switching rendering backends.
To draw into a texture your application already owns, use the [Direct3D 11 renderer](./d3d11-renderer.md).

## Environment and SDK

Direct3D 11 and Direct2D 1.1 are required.
API availability includes the Platform Update for Windows 7 SP1 or Windows 8 and later, but the required interfaces and render targets must also be creatable.
Transparent composition requires additional capabilities.
See Microsoft's [Platform Update documentation](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/platform-update-for-windows-7).
See the [physical-machine test procedure](./development/testing.md#prepare-a-physical-windows-machine) for checking display and transparency behavior that depends on the OS and GPU.

<a id="配布sdkと実行可能な例"></a>

From the repository root, create a separate SDK for each architecture.

```sh
make install-win32-d3d11-amd64 PREFIX="$PWD/build/sdk-d3d11-amd64"
export PKG_CONFIG_LIBDIR="$PWD/build/sdk-d3d11-amd64/lib/pkgconfig"
x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Wpedantic -Werror \
  build/sdk-d3d11-amd64/share/boxrig/examples/win32/d3d11_component.c \
  $(pkg-config --cflags --libs --static boxrig-win32-d3d11) \
  -o build/d3d11_component.exe
```

For i686, change the target, prefix, and compiler accordingly.
This module is a static library.
No additional boxrig DLL needs to be deployed.

## Place a component in a parent window

This function creates a GPU-rendered terminal child window.
It is the creation excerpt called after preparing COM, the parent window, terminal, and UI model.
After connecting allocation and frame processing, it displays text fed into core.
`native_result` receives detailed creation failure information.

```c
#include <boxrig/win32/d3d11_component.h>

boxrig_ui_status create_d3d11_child(HINSTANCE instance, HWND parent,
    boxrig_ui_model *model, HRESULT *native_result,
    boxrig_win32_terminal_component **component_out)
{
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
    graphics.creation_result = native_result;
    return boxrig_win32_d3d11_terminal_component_create_child(
        instance, parent, model, &config, &graphics, component_out);
}
```

The default requests a hardware device.
Failure does not automatically select WARP or the standard renderer.
Output pointers supplied at creation are used only during creation and are not retained by the component.
Use `boxrig_ui_status` to determine overall success or failure.

## Advance rendering and waiting

After synchronizing the model, call the normal `boxrig_win32_terminal_component_frame()`.
When `additional_dispatch` becomes false, wait for native messages and `activity_handle()` together.
Only after actually receiving a notification for that handle, call `dispatch_activity()` and advance the next frame.

Do not keep looping solely because `frame_pending` is true.
Perform creation, input handling, rendering, and destruction on the creating thread.
See [run_component in the full example](../../examples/win32/d3d11_component.c) for the waiting integration.

## Transparency and resizing

Distinguish background alpha from toplevel transparency.
Alpha in the component's presentation surface does not necessarily make the desktop visible through it.
The application owning the parent supplies the composition requirements.

The bundled `--transparent` example uses `WS_EX_NOREDIRECTIONBITMAP` on a non-layered parent.
See [Extended Window Styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles) for the meaning of the parent styles.
`--fallback` destroys the component and parent window, then creates a parent for standard rendering.
The parent style used for transparency is not carried over to opaque rendering.

For switching background transparency and whole-window opacity while retaining a normal parent, see the bundled terminal's [opacity control](../../examples/boxrig_term/win32/opacity.c).
The parent's owner draws its background and applies its attributes.

Pass size changes to the common `allocate()` function.
The last completed display is retained while a new render target is prepared.
Confirm native dimensions and terminal grid dimensions before reporting their respective completions.

## Diagnose failures and recover

`get_diagnostics()` records HRESULTs, composition failures, device and target generations, actual alpha, and accepted frame counts.
If only DirectComposition is unavailable, opaque rendering may still work.
A mandatory transparency request fails when it cannot be satisfied.

For managed device loss, the component recreates resources.
If recovery is impossible, the application destroys the component and any necessary parent, then creates another backend using the same terminal and model.
Place the decision to switch explicitly in application code.

## Runnable example

Running `d3d11_component.exe` on Windows displays a terminal with fixed text.
`--check` completes the first frame and exits.
`--transparent --fallback` recreates a transparent display using the standard renderer; `--warp` checks operation with explicitly selected WARP.

[Complete source](../../examples/win32/d3d11_component.c), [Win32 integration](./platforms/win32.md), [Documentation index](./index.md)
