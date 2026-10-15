# Platform support and verification scope

[日本語](../../ja/reference/platform-support.md)

Using boxrig core to interpret terminal output, boxrig UI to display it, and a PTY to run a shell each have different environment requirements.
These tables identify the OS, rendering, and input capabilities required by each configuration.
See [Compatibility](../compatibility.md) for the feature overview and the [platform guides](../index.md#integrating-into-an-application) for integration.

Requirements in code are listed separately from the methods used to check each feature.
Actual display and input availability also depend on the peer's capabilities, OS, and driver.

## Core, UI, and PTY requirements

| Layer | Requirements | Main verification method |
| --- | --- | --- |
| Core | C99 and the standard C library; Unicode, PNG, and compression dependencies are bundled into the static library | Portable core tests |
| Common UI | Core; no particular window system required | Model, input, layout, and clock unit tests |
| Platform UI | Rendering and input libraries listed below | Component, SDK, and screen tests |
| POSIX PTY | POSIX PTYs and nonblocking I/O | Session and child-process tests |
| Win32 PTY | A runtime providing ConPTY | Separate tests for OS-supplied and bundled implementations |

See [Building and installing](../building.md) for builds and [Sessions](../sessions.md) for transport integration.

## Linux and frontends

| Configuration | Build and integration requirements | Main verification method |
| --- | --- | --- |
| X11 | Xlib, Pango 1.50 or later, Cairo 1.18 or later, IBus 1.5.29 or later | Rendering, input, and clipboard tests under Xvfb |
| GTK 3 | GTK 3.24 or later, common Pango/Cairo/IBus dependencies | Widget and SDK tests on GDK X11/Wayland with Cairo/OpenGL |
| GTK 4 | GTK 4.14 or later, common Pango/Cairo/IBus dependencies | Widget and SDK tests on GDK X11/Wayland with Cairo/OpenGL |
| Native Wayland | wayland-client 1.22 or later, xkbcommon 1.6 or later, Pango/Cairo/IBus | Rendering and input tests under Weston, and protocol tests with simulated servers |
| OpenGL | OpenGL 3.3 core or OpenGL ES 3.0; libepoxy for GTK, EGL and related libraries for X11/Wayland | Software GL rendering tests; check the GPU and driver in the deployment environment |

Wayland builds use XML from pinned wayland-protocols 1.48.
wayland-server is used for tests.
xdg-shell, window decorations, and optional libdecor 0.2 or later are used by the bundled terminal, which owns a toplevel window.

The native Wayland component requires `wl_compositor`, `wl_shm`, and `wl_subcompositor`.
Their absence at creation means unavailable; losing them during operation is a fatal connection state.
Keyboard and pointer input additionally require a seat, IME requires text-input-v3, and copying requires the corresponding data-device/primary-selection protocols.
viewporter and fractional-scale work together for fractional scaling.
A feature is unavailable without its required protocols.

Interface version 2 of text-input-v3 supports clause attributes; version 1 provides conventional preedit.
Updating protocol definitions alone does not add capabilities missing from the peer.
See the [protocol source](https://github.com/wayland-mirror/wayland-protocols/blob/1.48/unstable/text-input/text-input-unstable-v3.xml) and [integration guide](../platforms/wayland.md).
Touch, drag and drop, and HTML/image clipboard contents are unsupported.

## Windows requirements by layer

| Feature | Requirements in code | Main verification method |
| --- | --- | --- |
| i686 core, common UI, and basic communication | Windows 2000 baseline; `_WIN32_WINNT=0x0500`, PE 5.0 | Artifact import audits and functional core, common UI, and session tests |
| The same base layer on amd64 | Windows XP x64 baseline; `_WIN32_WINNT=0x0502`, PE 5.2 | Artifact import audits and functional core, common UI, and session tests |
| Standard Win32 UI | DirectWrite from Vista SP2 plus Platform Update, or Windows 7 and later | Functional and rendering tests under Wine; check real Windows separately with P2 tests |
| OpenGL child window | Basic text-rendering requirements plus OpenGL 3.3 core, or ES 3.0 through a supported WGL ES extension | GPU, ICD, and pixel-format capabilities checked at creation |
| D3D11 renderer | Direct2D 1.1 from Windows 7 SP1 plus Platform Update or later; a BGRA-capable device | Standalone renderer tests |
| D3D11 component | DXGI 1.2 for HWND swapchains; DirectComposition from Windows 8 or later for alpha composition | Component and SDK tests; check final composition on physical machines |
| OS-supplied ConPTY | Windows 10 1809 or later | Dynamically resolved; unavailable when absent |
| System backdrop | Windows 11 build 22621 or later | Background-effect capabilities queried at runtime |

The Windows 2000 base layer does not include native UI or ConPTY.
Without newer DirectWrite font-fallback APIs, the implementation uses the base factory and font collection.
Newer DPI APIs also have runtime fallback paths.
The [Windows 7 Platform Update](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/platform-update-for-windows-7) does not provide DirectComposition, so D3D11 rendering and background-alpha requirements are separate.

Run `make test-win32-core-artifacts test-win32-ui-artifacts` to audit base-ABI artifact formats and imports.
Import audits and execution under Wine alone do not establish runtime verification on each older OS.
See [Distribution](./distribution.md) for choosing OS-supplied or bundled ConPTY.

## Rendering, transparency, and background effects

| Configuration | Background-only alpha | OS blur and related effects |
| --- | --- | --- |
| X11 | Requires an ARGB visual and compositor | Requests to a supporting window manager |
| GTK | Depends on the GDK target and compositor | Optional provider; GTK 4 Wayland integration has additional requirements |
| Native Wayland | The compositor composites surface alpha | Requires a supported background-effect protocol |
| Standard Win32 rendering | Requires alpha-capable DirectWrite rendering and conditions such as a layered child | Requires DWM and target-OS support |
| Win32 OpenGL | Depends on the WGL pixel format and final window composition | Same as above |
| Win32 D3D11 | Depends on a composition swapchain and DirectComposition | Same as above |

Setting a color's alpha does not necessarily reveal the desktop behind it.
This also differs from transparency of the whole window, including text.
See [Backgrounds](../background.md) and [Rendering options](../renderers.md) for requested and actual states.
API success under Wine is not treated as equivalent to final composition on Windows.

## IME, notifications, clipboard, and accessibility

### IME routes

| Route | Surrounding text and editing | Conditions and limitations |
| --- | --- | --- |
| GTK 3/4 | Document access and replacement through a synchronous owner | GTK 3's API does not pass the selection anchor |
| GTK 4 with the normal IBus module | Same as above | The IBus 1.5.29-rc2 custom-widget route does not deliver the anchor to the engine |
| Direct IBus | Passes cursor and anchor and displays candidates inside the terminal | Requires an IBus connection |
| Direct fcitx 5 | Cursor and anchor, synchronous edits, formatted preedit, and candidate pages | Requires a running 5 daemon; CLIENT ownership remains unconfirmed until a panel notification arrives |
| Direct fcitx 4 | Cursor and anchor, synchronous edits, and a string panel containing preedit | Keyboard candidate operation; SYSTEM uses formatted preedit |
| Native Wayland text-input-v3 | Treats deletion and commit as one replacement | Requires the peer protocol |
| XIM | Supplies context and character-based editing to supporting servers | Unavailable unless the server advertises string-conversion support |
| Win32 | DOCUMENTFEED and reconversion of selected text | Ordinary IME commits go to the PTY; the common synchronous owner alone does not change this |

Without a registered application document, ordinary committed text goes to the PTY.
On GTK, Wayland, and supported XIM routes with context-only publication, document deletion is rejected and the following commit is not forwarded to the PTY.
Editing requires a registered synchronous owner.
See [Input and IME](../input-ime.md).

Context supplied to GTK/Wayland must not contain NUL.
The window supplied to Wayland/XIM is at most 4,000 bytes; a selection that cannot fit in full is unsupported.
Requests unrepresentable in the XIM locale are also rejected.
GTK decorations use UTF-8 scalar boundaries and discard only invalid ranges.
Decorations beyond 256 spans are omitted.
Wayland version 2 limits distinct ranges within one done event to 128; valid text and commits are retained on overflow.

GTK automatic selection gives priority to a nonempty `GTK_IM_MODULE`.
GTK 4 Wayland clause display requires both GTK runtime 4.23.4 or later and text-input-v3 version 2; a direct IBus/fcitx 5/4 route is available when those conditions are not met.
See the [IME selection implementation](../../../src/gtk/ime_probe.c) for route selection and [Input and IME](../input-ime.md) for context and editing requirements.

The normal GTK 4 route in IBus 1.5.29-rc2 has cancellation-notification limitations in its default synchronous mode.
The bundled terminal sets `IBUS_ENABLE_SYNC_MODE=2` only when it is unset.
The library does not modify the process environment.
Sources are [IBus synchronous processing](https://github.com/ibus/ibus/blob/1.5.29-rc2/src/ibusinputcontext.c#L1557) and the [GTK module](https://github.com/ibus/ibus/blob/1.5.29-rc2/client/gtk2/ibusimcontext.c#L945).

### Notifications and clipboard

The bundled terminals on all five platforms pass OSC 99 to notification providers.
Linux uses asynchronous freedesktop Notifications through GLib/GIO; Win32 uses the Shell notification area.
The default is rejection; after permission is enabled, the limit starts with five tokens and refills one every two seconds up to five.
Resending the same successfully delivered nonempty ID and content within one second does not produce a duplicate OS request.
Unknown visibility is treated as visible.

Win32 notification titles are limited to 63 UTF-16 code units and bodies to 255.
Excess text is truncated without splitting surrogate pairs.
OS settings may prevent display.
See [NOTIFYICONDATA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw) and [freedesktop Notifications](https://specifications.freedesktop.org/notification-spec/latest/).

| Configuration | User copying and pasting | OSC 52 in the bundled terminal |
| --- | --- | --- |
| X11 / GTK 3/4 | PRIMARY and CLIPBOARD | Writes to supported selections; reads denied by default |
| Native Wayland | Selections subject to protocol and input-serial requirements | No provider connected; writes return UNAVAILABLE and reads DENIED |
| Win32 | `CF_UNICODETEXT`; newline conversion as needed | `c/s/0` map to the system clipboard; reads denied by default |

The UI paste limit defaults to 8 MiB, with 4 KiB retry chunks.
The bundled terminal explicitly aligns these with core's 32 KiB/64 KiB settings.
It normalizes newlines to LF, removes control characters except LF and tab, and continues the same paste when output must wait.
On Win32, the old clipboard contents cannot be guaranteed to survive a `SetClipboardData()` failure after successful `EmptyClipboard()`.
This follows [Windows clipboard operations](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations).

GTK 3 sets `ATK_ROLE_TERMINAL`; GTK 4 sets `GTK_ACCESSIBLE_ROLE_TERMINAL`.
Text, changes, and selections are not exposed through AtkText or GtkAccessibleText.
Setting the role alone is not full screen-reader support.

## Verification methods and deployment checks

See [Testing](../development/testing.md) for the regular suite and physical Windows P2 tests.
Tests using Xvfb, Weston, Wine, and simulated callbacks check rendering, input, and API state transitions.
Also check OS-provided IME candidate placement, desktop transparency composition, and GPU driver behavior in the deployment environment.

OS candidate placement on normal GTK 4 Wayland depends on [IBus integer scaling](https://github.com/ibus/ibus/blob/1.5.29-rc2/client/gtk2/ibusimcontext.c#L1585) and [GNOME Shell 46 logical-coordinate handling](https://github.com/GNOME/gnome-shell/blob/46.0/js/ui/ibusCandidatePopup.js#L211).
Check candidate placement at fractional scales, including terminal movement and focus changes.

For XIM string conversion, successful simulated callbacks are not treated as successful communication with an external server.
The reply handling in libX11 1.8.7 has NULL-reference and copy-source problems.
The relevant file is `modules/im/ximcp/imCallbk.c` in the [official source](https://www.x.org/releases/individual/lib/libX11-1.8.7.tar.xz).
IBus XIM connections without string-conversion support continue accepting input without that capability.

## External specification sources

In addition to the sources above, consult [DirectWrite](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nn-dwrite-idwritefactory), [ConPTY](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole), [DWM background effects](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type), [GTK surrounding text](https://docs.gtk.org/gtk4/method.IMContext.set_surrounding_with_selection.html), and [Xlib string conversion](https://www.x.org/releases/current/doc/libX11/libX11/libX11.html#String_Conversion_Callback).
Check both external API availability and the boxrig-specific conditions in these tables.

[Documentation index](../index.md)
