# Licenses for boxrig and third-party components

This document follows the [Japanese source](https://github.com/kekyo/boxrig/blob/main/packaging/licenses/README_ja.md).
boxrig's own code is licensed under MIT; see `LICENSE`.
Embedded libraries and generated data retain their own copyright notices and distribution conditions.
Keep the notices and license texts applicable to the artifacts you redistribute.
This directory collects notices for the SDK configurations on the same operating system.
Using an individual library does not necessarily use every component listed below.

## Embedded code and data

| Component | Use | License text |
| --- | --- | --- |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) | Unicode processing in core | `utf8proc-LICENSE.md`, including both MIT and Unicode data notices |
| [libpng](https://github.com/pnggroup/libpng) | PNG handling in core | `libpng-LICENSE`, PNG Reference Library License |
| [zlib](https://github.com/madler/zlib) | Compression in core | `zlib-LICENSE` |
| [Unicode 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/) | UI word-boundary tables | `Unicode-LICENSE.txt`, Unicode License V3 |
| [WHATWG Encoding](https://encoding.spec.whatwg.org/) | Shift_JIS conversion tables in core | `WHATWG-Encoding-LICENSE`; portions incorporated into code use BSD-3-Clause |

Acknowledgment in binary distributions is not mandatory for libpng or zlib, but the SDK includes their license texts.
Keep both utf8proc's Unicode data notices and the UI's Unicode License V3.
The WHATWG text includes CC BY 4.0 for the specification and BSD-3-Clause for portions incorporated into code.
boxrig's MIT license does not replace these licenses.

## Linux

Linux distributions include `cardio-LICENSE` for [cardio](https://github.com/kekyo/cardio), used by the demo's URL launcher,
and `Wayland-protocols-COPYING` plus `Wayland-protocols-NOTICES.md` for [Wayland protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols).
The latter preserves each copyright and permission notice from the XML inputs used to generate code.

System shared libraries include GTK 3, GTK 4, Pango, GLib/GObject/GIO, IBus, ATK and glibc.
These libraries and their use are covered by the LGPL; see `LGPL-2.1.txt`.
Cairo offers LGPL 2.1 or MPL 1.1; this distribution selects LGPL 2.1.
See [GTK](https://docs.gtk.org/gtk4/), [Cairo](https://www.cairographics.org/) and
[LGPL 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).

Distributed executables are intended to dynamically link to libraries installed on the user's system.
Replacement with interface-compatible modified libraries, modification for the user's own use and reverse engineering to debug those modifications are permitted.
Bundling those libraries or linking them statically requires separately satisfying the applicable source and relinking requirements.

## Windows and GCC runtimes

Windows distributions include MinGW-w64 runtime notices in `MinGW-w64-LICENSE.txt`.
For GCC runtimes, `GPL-3.0.txt` and `GCC-Runtime-Library-Exception.txt` are included.
The Linux demo's C++ code also uses GCC runtimes.
Builds meeting the [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html) may distribute boxrig's own code under MIT.
Redistribution of a runtime library itself requires satisfying its own source distribution and other conditions.
See the [GCC explanation](https://www.gnu.org/licenses/gcc-exception-3.1-faq.html).

Normal Windows configurations use OS-provided ConPTY without bundling Microsoft executables.
When choosing packaged ConPTY, retain `LICENSE.conpty.txt` and `NOTICE.conpty.md` with the runtime files.
The upstream NOTICE also contains notices for other Windows Terminal configurations.
The pinned package is [ConPTY 1.24.260710001](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.24.260710001),
corresponding to [Windows Terminal v1.24.11911.0](https://github.com/microsoft/terminal/releases/tag/v1.24.11911.0).

## Provenance and source use

`sources.json` records the source URLs and SHA-256 hashes of upstream license texts copied into the repository's `packaging/licenses/` directory.
The MinGW-w64 notice corresponds to 11.0.1 and the GCC texts to the 13.2.0 upstream distribution.
When distributing artifacts built with another toolchain, check that version's conditions.
Submodule references identify dependency versions; data provenance identifies generated-data versions.

When using boxrig as a submodule or from GitHub's source ZIP, retain the licenses accompanying dependencies and data.
Redistributed artifacts must carry the notices applicable to the selected configuration.
Partial SDK uninstallation retains license notices shared with other modules.
