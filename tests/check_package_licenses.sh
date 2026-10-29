#!/bin/sh
set -eu
make_command=$1
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM

# Check the installed distribution, including unchanged upstream notices.
for arch in amd64 i686; do
    prefix="$root/$arch/opt/boxrig"
    "$make_command" --no-print-directory "install-core-win32-$arch" \
        DESTDIR="$root/$arch" PREFIX=/opt/boxrig \
        CONPTY_BUILD_DIR="$root/no-conpty" \
        CONPTY_PACKAGE_URL=file:///boxrig-test-must-not-download-conpty \
        > "$root/install-$arch.log" 2>&1 || {
        cat "$root/install-$arch.log"
        exit 1
    }
    licenses="$prefix/share/licenses/boxrig"
    cmp LICENSE "$licenses/LICENSE"
    cmp deps/utf8proc/LICENSE.md "$licenses/utf8proc-LICENSE.md"
    cmp deps/libpng/LICENSE "$licenses/libpng-LICENSE"
    cmp deps/zlib/LICENSE "$licenses/zlib-LICENSE"
    cmp resources/unicode/17.0.0/LICENSE.txt "$licenses/Unicode-LICENSE.txt"
    cmp data/encoding/whatwg/LICENSE "$licenses/WHATWG-Encoding-LICENSE"
    test -s "$licenses/README_ja.md"
    test -s "$licenses/README.md"
    test -s "$licenses/MinGW-w64-LICENSE.txt"
    test -s "$licenses/GPL-3.0.txt"
    test -s "$licenses/GCC-Runtime-Library-Exception.txt"
    test ! -e "$prefix/bin/conpty.dll"
    test ! -e "$licenses/ConPTY-NOTICE.md"
done

# Installed DLLs still embed codecs after the standalone core is removed.
"$make_command" --no-print-directory install-win32-opengl-amd64 uninstall-core-win32-amd64 \
    DESTDIR="$root/amd64" PREFIX=/opt/boxrig \
    CONPTY_BUILD_DIR="$root/no-conpty" \
    CONPTY_PACKAGE_URL=file:///boxrig-test-must-not-download-conpty \
    > "$root/partial-remove.log" 2>&1 || {
    cat "$root/partial-remove.log"
    exit 1
}
test -s "$root/amd64/opt/boxrig/bin/boxrig-win32-ogl.dll"
cmp data/encoding/whatwg/LICENSE "$root/amd64/opt/boxrig/share/licenses/boxrig/WHATWG-Encoding-LICENSE"
test ! -e "$root/no-conpty"

for configuration in static shared-gtk3 shared-gtk4 shared-native shared-opengl shared-effects shared; do
    if [ "$configuration" = static ]; then
        set -- install-gtk3 install-gtk4 install-wayland install-render-ogl \
            install-x11-opengl install-wayland-opengl install-notification \
            install-background-gtk3 install-background-gtk4
    else
        set -- "install-$configuration"
    fi
    "$make_command" --no-print-directory "$@" \
        DESTDIR="$root/linux-$configuration" PREFIX=/opt/boxrig \
        > "$root/install-linux-$configuration.log" 2>&1 || {
        cat "$root/install-linux-$configuration.log"
        exit 1
    }
    licenses="$root/linux-$configuration/opt/boxrig/share/licenses/boxrig"
    cmp deps/cardio/LICENSE "$licenses/cardio-LICENSE"
    cmp deps/wayland-protocols/COPYING "$licenses/Wayland-protocols-COPYING"
    cmp packaging/licenses/LGPL-2.1.txt "$licenses/LGPL-2.1.txt"
    test -s "$licenses/README_ja.md"
    test -s "$licenses/README.md"
    # These owners and the text-input permission are absent from the common COPYING.
    rg -q 'Kenny Levinsen' "$licenses/Wayland-protocols-NOTICES.md"
    rg -q 'Purism SPC' "$licenses/Wayland-protocols-NOTICES.md"
    rg -q 'Xaver Hugl' "$licenses/Wayland-protocols-NOTICES.md"
    rg -q 'written prior permission' "$licenses/Wayland-protocols-NOTICES.md"
    test ! -e "$licenses/MinGW-w64-LICENSE.txt"
done
printf '%s\n' 'PASS installed third-party licenses (Linux, Windows amd64/i686)'
