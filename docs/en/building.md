# Building and installing

[日本語](../ja/building.md)

This guide builds boxrig from source and prepares the headers and libraries for your application.
It uses GCC and GNU Make on Linux, with MinGW-w64 for Windows cross-builds.

Use boxrig core to interpret terminal output without a display or to implement rendering yourself.
To embed a terminal in an existing window, add boxrig UI and the library for your platform.
Communication with a shell also requires the session and PTY libraries.
See [Library structure](../../README.md#library-structure) to choose a configuration and [Getting started](./getting-started.md) for programs to try after building.

## Obtain sources and dependencies

```sh
git clone --recursive https://github.com/kekyo/boxrig.git
cd boxrig
```

If you already have the source tree, run `git submodule update --init` from the repository root.
This places pinned dependency sources in `deps/`.
You do not need to modify external code directly.

The Makefile uses the following main development dependencies.
Version requirements come from this source tree's build definitions.
Install the OS development packages needed for your configuration.

| Purpose | Requirements |
| --- | --- |
| Common | GCC, GNU Make, Git, pkg-config |
| X11 | Xlib, XComposite, XRender, XFixes, GLib/GIO |
| Linux text rendering | Pango 1.50 or later, Cairo 1.18 or later, PangoCairo, PangoFT2, Fontconfig |
| GTK 3/4 | GTK 3.24 or later / GTK 4.14 or later, IBus 1.5.29 or later, Wayland client; libepoxy for the default GL-enabled build |
| Native Wayland | Wayland client/server 1.22 or later, xkbcommon 1.6 or later, IBus 1.5.29 or later, wayland-scanner, wayland-cursor |
| OpenGL | OpenGL and EGL development packages; Wayland also uses wayland-egl |
| Windows | MinGW-w64 for amd64 and i686; matching g++ for sample code written in C++ |
| Generating Linux SDK license information | Node.js, npm, and development dependencies for the [code generation tools](../../tools/codegen/package.json) |
| Creating distribution packages | Bash, tar, gzip, zip; Podman and QEMU/binfmt for multiple Linux targets |

The shared Makefile queries dependencies beyond the requested target while reading its configuration.
You may see missing pkg-config dependency messages even when you do not link the corresponding UI library.
First check the intended configuration and the target you ran.

Core bundles utf8proc for Unicode processing and libpng and zlib for PNG images into `libboxrig.a`.
Applications using only core do not need to link X11 or GTK.
Consult the table above for the system libraries needed to build and run each UI configuration.

Before installing a Linux SDK, prepare the tools that generate license notices.

```sh
npm ci --prefix tools/codegen
```

Wayland extension definitions are generated from the pinned [wayland-protocols 1.48](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tags/1.48).
Generated headers and C files go in `build/` and are not part of the public SDK.

## Build for Linux

To build the Linux libraries and demos together, run:

```sh
make native
```

Running `make` without a target also builds Windows amd64 and i686.
Specify `native` if you do not need Windows.

| Required output | Command | Main output path |
| --- | --- | --- |
| Core only | `make build/libboxrig.a` | `build/libboxrig.a` |
| GTK 3 demo | `make boxrig-term-gtk3` | `build/boxrig_term/gtk3/boxrig_term` |
| GTK 4 demo | `make boxrig-term-gtk4` | `build/boxrig_term/gtk4/boxrig_term` |
| Native Wayland demo | `make boxrig-term-wayland` | `build/boxrig_term/wayland/boxrig_term` |
| X11 demo | `make build/boxrig_term/x11/boxrig_term` | The path in the command |

The common UI, communication, and POSIX libraries are `build/libboxrig-ui.a`, `build/libboxrig-session.a`, and `build/libboxrig-posix.a` respectively.
Platform-specific static libraries also go in `build/`.

### Use Linux shared libraries

Build and install shared core, UI, session, and POSIX libraries with:

```sh
make shared-base BUILD_DIR=/tmp/boxrig-build
make install-shared-base BUILD_DIR=/tmp/boxrig-build PREFIX="$HOME/.local"
pkg-config --cflags --libs boxrig-ui boxrig-posix
```

Products go in `BUILD_DIR/shared/`, or `build/shared/` by default.
Dedicated PIC objects keep these outputs separate from ordinary static builds.
Installation includes shared and static libraries; shared SONAMEs include `libboxrig.so.0`.
Distribute the required `.so.0` files together with their boxrig dependencies.
Libraries find sibling dependencies in the same directory, but your application must locate that directory through the standard loader path, its RUNPATH, or `LD_LIBRARY_PATH`.

Set `PREFIX`, `LIBDIR`, `INCLUDEDIR`, and `PKGCONFIGDIR` to absolute paths without whitespace.
Use `DESTDIR` for staging. Generated pkg-config paths are relative to the installation, so the SDK can be relocated as a unit.
For a custom location, add its `lib/pkgconfig` directory to `PKG_CONFIG_PATH`.
Use `pkg-config --static --libs` to include static dependencies.
When shared libraries are present in the same search path, selecting static linkage also requires compiler/linker options.
Public APIs are the functions declared in installed headers. Applications must not use symbols in `BOXRIG_PRIVATE_0`.
Bundled libpng, zlib, and utf8proc symbols are not exported and remain independent of the codecs used by the application or GTK.

Use `make shared` and `make install-shared` to include shared GTK libraries.
For GTK3 alone, use `shared-gtk3` and `install-shared-gtk3`; replace the suffix with `gtk4` for GTK4 alone.
Each includes the four base libraries. `GTK_OPENGL=0` selects a Cairo-only build.
Linux distribution SDKs include these six shared libraries alongside static archives.
Set `BOXRIG_VERSION` to control the version of the six generated `.pc` files; distribution packages use the version obtained from `screw-up`.

## Cross-build for Windows

```sh
make win32
```

Use `make win32-amd64` or `make win32-i686` if you need only one architecture.
Libraries go in `build/win32/amd64/` or `build/win32/i686/`, and terminal demos in `build/boxrig_term/win32/<arch>/`.

Normal PTY connections use the ConPTY supplied with Windows.
Building or installing the standard SDK does not require downloading ConPTY from NuGet.
See [Compatibility](./compatibility.md) for Windows runtime requirements.

## Build distribution packages

Linux packages are built in Podman containers for each distribution and CPU.
The following nine targets are provided. Windows packages continue to use the host's MinGW-w64 toolchains for amd64 and i686.

| Distribution | Release | CPUs |
| --- | --- | --- |
| Debian | 13 (trixie) | x86_64, i686, arm64, armv7l, riscv64 |
| Ubuntu | 24.04 | x86_64, arm64 |
| Ubuntu | 26.04 | x86_64, arm64 |

The host needs Podman, GNU Make, Git, tar, Node.js 22.12 or later, npm, and npx.
Use a Node.js version that satisfies the [code generation dependencies](../../tools/codegen/package-lock.json).
Targets with a different CPU from the host also need QEMU and registered binfmt handlers.
See [Podman's multi-architecture build documentation](https://docs.podman.io/en/v4.9.3/markdown/podman-build.1.html) for details.
To also create Windows packages, prepare the cross-build environment described above.

First prepare images containing the build dependencies, then create packages.

```sh
./prereq.sh
./build_package.sh
```

`prereq.sh` reuses existing images. Run `./prereq.sh --force` to refresh their dependencies.
To narrow the targets, pass matching `--distro`, `--release`, and `--arch` filters to both commands.

```sh
./prereq.sh --distro debian --arch i386,armhf,riscv64
./build_package.sh --target linux --distro debian --arch i386,armhf,riscv64 --jobs 8
```

Each filter accepts a comma-separated list.
`amd64`, `i386`, `aarch64`, and `armhf` are aliases for `x86_64`, `i686`, `arm64`, and `armv7l`, respectively.
Release aliases `13`, `noble`, and `resolute` select `trixie`, `24.04`, and `26.04`.
Use `--list-targets` to list the selection without building images or packages.

`build_package.sh` initializes submodules and installs code generation dependencies with `npm ci`.
Output goes to `artifacts/` at the repository root. The default selection creates nine Linux archives and two Windows archives.
The version is obtained by running [screw-up's format command](https://github.com/kekyo/screw-up#format-command) as `npx --yes screw-up format -f -e '{version}'`.

| File name | Target |
| --- | --- |
| `boxrig-<version>-linux-<distro>-<release>-<arch>.tar.gz` | Linux SDK for the named distribution and CPU |
| `boxrig-<version>-win32-amd64.zip` | 64-bit Windows |
| `boxrig-<version>-win32-i686.zip` | 32-bit Windows |

For example, the Ubuntu 24.04 x86_64 archive is named `boxrig-0.1.0-linux-ubuntu-24.04-x86_64.tar.gz`.
The extracted directory name includes the same version number.
Choose the Linux archive that matches the distribution and CPU where it will be used.

Each archive contains the target OS's SDK, terminal demos, emitter, English README, and license documents.
Example source files are not included. See the repository's [examples/](../../examples/) directory.
Japanese documents are available online through the README and are not included in the archives.
Linux terminal demos are `bin/boxrig_term_x11`, `boxrig_term_gtk3`, `boxrig_term_gtk4`, and `boxrig_term_wayland`.
Windows packages include `bin/boxrig_term.exe` and its launcher.

Targets run serially. `--jobs` controls Make parallelism within one target, defaulting to the `JOBS` environment variable or the host CPU count when unset.
Existing archives with matching names are replaced after all selected targets succeed.
If a prerequisite image is missing, the script exits with the `prereq.sh` command needed to prepare it.

Use `./build_package.sh --target win32` to create only the Windows packages.
Use `./build_package.sh --target native` to create a Linux package with the host's development environment.
The latter produces `boxrig-<version>-linux-<arch>.tar.gz`, where `<arch>` is the result of `uname -m`.

Run `bash tests/check_package_scripts.sh` for script checks without compilation, or `make -j1 test` for the full suite.
The full suite's package consumption checks cover host Linux and Windows packages.

## Install the modules you need

Set the installation destination with `PREFIX`.
For a local trial without writing to system directories, use a path under the repository's `build/` directory.

```sh
make install-gtk4 PREFIX="$PWD/build/sdk-gtk4"
```

| Configuration | Installation target | pkg-config name |
| --- | --- | --- |
| GTK 3 | `install-gtk3` | `boxrig-gtk3` |
| GTK 4 | `install-gtk4` | `boxrig-gtk4` |
| Native Wayland | `install-wayland` | `boxrig-wayland` |
| OpenGL rendering API | `install-render-ogl` | `boxrig-render-ogl` |
| X11 OpenGL component | `install-x11-opengl` | `boxrig-x11-ogl` |
| Wayland OpenGL component | `install-wayland-opengl` | `boxrig-wayland-ogl` |
| Win32 core | `install-core-win32-amd64` | `boxrig` |
| Win32 default UI | `install-win32-amd64` | `boxrig-win32` |
| Win32 OpenGL | `install-win32-opengl-amd64` | `boxrig-render-ogl`, `boxrig-win32-ogl` |
| Direct3D 11 rendering API | `install-render-d3d11-amd64` | `boxrig-render-d3d11` |
| Direct3D 11 component | `install-win32-d3d11-amd64` | `boxrig-win32-d3d11` |

For Windows i686, replace the target suffix with `i686`.
Use separate installation destinations for different architectures.

Notifications and GTK/Wayland background effects have additional installation targets.
Consult the [background guide](./background.md) and public headers when using them.
For standalone configurations without an installation target, use the libraries built in the source tree.
To prepare the target OS's SDK as a whole, use `build_package.sh` as described above.

## Link from an installed SDK

Public headers are installed in `include/boxrig/`, libraries in `lib/`, and pkg-config files in `lib/pkgconfig/`.

```sh
export PKG_CONFIG_PATH="$PWD/build/sdk-gtk4/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --cflags --libs --static boxrig-gtk4
```

Most current boxrig libraries are static archives.
Use `--static` to include the dependencies those archives require.
You also need development packages for the system libraries listed by the command.

For Windows, point pkg-config only at the target installation to avoid mixing in host libraries.

```sh
make install-win32-amd64 PREFIX="$PWD/build/sdk-win64"
PKG_CONFIG_LIBDIR="$PWD/build/sdk-win64/lib/pkgconfig" \
  pkg-config --cflags --libs --static boxrig-win32
```

The examples installed by `make install-*` vary by module.
`share/boxrig/examples/` contains items such as runnable Win32 examples and external scrollbar headers for each platform.
It does not include every demo.

Statically linking the Win32 UI requires no additional boxrig DLLs.
OS-supplied ConPTY also needs no additional DLL or EXE, but an optional boxrig DLL must be distributed if you choose to use it.
Explicitly choosing bundled ConPTY requires DLLs for the target architecture, the host matching the OS, and license notices.
See the [ConPTY distribution files](../../deps/conpty/README.md).

## Select optional features

The default GTK build includes both Cairo and OpenGL.
Build a Cairo-only configuration as follows.

```sh
make GTK_OPENGL=0 boxrig-term-gtk3 boxrig-term-gtk4
```

Specify `GTK_OPENGL=1` again to return to a GL-enabled build.
The library configuration defaults to Cairo, while the bundled demos prefer GL.
Distinguish the backends included at build time from the backend selected at runtime.

The native Wayland demo detects libdecor 0.2 or later with the default `WAYLAND_LIBDECOR=auto`.
`1` requires it, and `0` disables it.
libdecor supplies demo decorations and is not included in the normal Wayland component SDK.

## When a build fails

For pkg-config errors, check the availability and versions of the named development packages.
If a Windows compiler is missing, install MinGW-w64 or select Linux targets only.

The build treats warnings as errors.
Before disabling a warning, check the compiler, dependency versions, and command used.
Also make sure you are not mixing source-tree files with headers from another SDK.

[Back to the documentation index](./index.md)
