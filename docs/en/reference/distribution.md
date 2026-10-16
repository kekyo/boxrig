# Distributing applications that use the SDK

[日本語](../../ja/reference/distribution.md)

The files needed to distribute a boxrig application depend on whether it uses core alone or adds UI and PTY support.
This page introduces the currently available SDKs, then explains link-time and runtime dependencies, OS-specific bundled files, and licenses.
See [Building and installing](../building.md) to create an SDK and the [SDK README](../../../README_pack.md) for a first example.

## Current SDKs and modules

An SDK installs headers, libraries, pkg-config definitions, examples, and license documents into a prefix of your choice.
This is not a claim that a single archive containing every feature is available for download.

| Configuration | Installation targets and pkg-config names |
| --- | --- |
| Linux shared base libraries | `install-shared-base`; `boxrig`, `boxrig-ui`, `boxrig-session`, `boxrig-posix` |
| Linux shared GTK libraries | `install-shared-gtk3`/`install-shared-gtk4`, or `install-shared` for both; `boxrig-gtk3`/`boxrig-gtk4` and the four base libraries |
| GTK 3/4 | `install-gtk3`/`install-gtk4`, `boxrig-gtk3`/`boxrig-gtk4` |
| Native Wayland | `install-wayland`, `boxrig-wayland` |
| Common OpenGL renderer | `install-render-ogl`, `boxrig-render-ogl` |
| X11/Wayland OpenGL component | `install-x11-opengl`/`install-wayland-opengl`, `boxrig-x11-ogl`/`boxrig-wayland-ogl` |
| Windows core and standard component | `install-core-win32-{amd64,i686}`, `install-win32-{amd64,i686}`; `boxrig`, `boxrig-win32` |
| Windows OpenGL | `install-win32-opengl-{amd64,i686}`; `boxrig-render-ogl`, `boxrig-win32-ogl` |
| Windows D3D11 | `install-render-d3d11-{amd64,i686}`, `install-win32-d3d11-{amd64,i686}`; `boxrig-render-d3d11`, `boxrig-win32-d3d11` |

The existing install targets provide static libraries.
Linux shared targets and distribution packages also include `.so.0` libraries for core, UI, session, POSIX, GTK3, and GTK4.
Windows OpenGL also has a DLL and import-library configuration.
Current D3D11 installation uses static libraries.
Use a separate prefix for each target ABI.

## Link-time and runtime dependencies

Headers go in `include/boxrig/`; libraries and `.pc` files in `lib/` and `lib/pkgconfig/`; examples in `share/boxrig/examples/`; and license documents in `share/licenses/boxrig/`.
Included examples vary by module.

The `.pc` files derive their prefix from their own location, so you can move the whole directory tree.
The six Linux shared `.pc` files accept `BOXRIG_VERSION` and match the release version in distribution packages. Other `.pc` files use `0.0.0`.
For Windows linking, set `PKG_CONFIG_LIBDIR` to the target SDK only and avoid mixing in host Linux `.pc` files.
Retrieve additional static-link dependencies with `pkg-config --static --libs`.

Linking a static archive does not embed system shared libraries such as GTK, Pango, or Cairo into the executable.
Runtime dependencies vary with the selected configuration.

Core's independence from window systems does not mean it contains no third-party code.
utf8proc, libpng, and zlib are compiled into core, so check [Third-party licenses](#third-party-licenses-and-sources) even when distributing core alone.

## Distribute on Linux

Core's Unicode, PNG, and compression dependencies are included in `libboxrig.a` and `libboxrig.so.0`; codec symbols are not exported from the shared library.
When using shared libraries, place the required boxrig `.so.0` files together and make that directory available through your application's RUNPATH, `LD_LIBRARY_PATH`, or the standard loader path.
Applications must not call `BOXRIG_PRIVATE_0` functions absent from public headers. Keep the SDK's libraries at the same version.
UI applications require shared libraries for the selected X11, GTK, Wayland, Pango, Cairo, IBus, and other facilities.
OpenGL also requires the target environment's EGL/GL libraries and drivers.

The bundled terminal uses cardio for launching URLs, while embedding applications can choose their own host integration.
Core-only applications do not need every dependency of the bundled terminal.

For distribution licenses, check the configuration you actually use against the audited [third-party license documentation](../../../packaging/licenses/README.md).
Using a system shared library has different requirements from bundling or statically linking that library.
The [LGPL 2.1 text](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) is also available.

## Distribute on Windows

Standard static linking requires no boxrig DLL.
If you choose the OpenGL DLL configuration, bundle the linked DLLs and required runtimes for the matching ABI.
Check imports, including dependencies added by your executable.

The bundled terminal uses `boxrig_term_launcher.exe` alongside `boxrig_term.exe`.
This launcher starts child processes for the demo and is not required by all SDK applications.

DirectWrite, DWM, Direct3D, and OpenGL are provided by the OS or drivers.
Deploying boxrig files alone does not add these capabilities to an older OS.
See [Platform support](./platform-support.md) for background-transparency and GPU-rendering requirements.

## Choose OS-supplied or bundled ConPTY

The default, `BOXRIG_WIN32_PTY_RUNTIME_SYSTEM`, dynamically loads OS-supplied ConPTY.
This configuration does not bundle Microsoft DLLs or executables.

| Selection | Behavior |
| --- | --- |
| SYSTEM | Use the OS-supplied implementation |
| PACKAGED_PREFERRED | Prefer the bundled implementation in the specified directory; fall back to the OS when the relevant files or capabilities are unavailable |
| PACKAGED_REQUIRED | Use only the bundled implementation; do not fall back to the OS on failure |

PREFERRED does not hide every failure.
It does not switch for errors such as permissions or resource exhaustion, or to restart an already started child process.
Diagnostic information about host candidates is also distinct from proof of the process actually launched.
See [pty.h](../../../include/boxrig/win32/pty.h) for configuration definitions.

The optional `make conpty-runtime-assets` downloads ConPTY 1.24.260710001, pinned in the Makefile, and verifies its SHA-256.
Match `conpty.dll` to the application's ABI and `OpenConsole.exe` to the OS architecture.
When distributing, preserve the required x86/x64/arm64 host layout and include `LICENSE.conpty.txt` and `NOTICE.conpty.md`.
Sources are the [package](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.24.260710001) and [corresponding upstream release](https://github.com/microsoft/terminal/releases/tag/v1.24.11911.0).

## Third-party licenses and sources

boxrig uses the [MIT License](../../../LICENSE).
Preserve the original terms for third-party code and data.
The SDK's `share/licenses/boxrig/` contains material for utf8proc, libpng, zlib, Unicode, WHATWG Encoding, and configuration-specific Wayland, cardio, MinGW-w64, GCC, and other dependencies.

Source URLs and hashes are in [sources.json](../../../packaging/licenses/sources.json), and configuration notes are in the [third-party license documentation](../../../packaging/licenses/README.md).
The [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html) also applies to GCC runtime components.
For distribution with another toolchain version, compare against the original license texts for that version.

## Distribution features not yet provided

Create SDKs with Makefile install targets and archives with `build_package.sh`.
Linux shared libraries have SONAMEs and symbol versions, but cross-version ABI compatibility guarantees and automated compatibility checks are not provided.

[Documentation index](../index.md)
