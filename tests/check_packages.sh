#!/bin/bash
# Build archives through the public entry point and consume the extracted SDKs.
set -euo pipefail
repository=$(pwd)
compiler=$1
amd64_compiler=$2
i686_compiler=$3
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
packages="$repository/artifacts"
version=$(npx --yes screw-up format -f -e '{version}')
linux_name="boxrig-$version-linux-$(uname -m)"
amd64_name="boxrig-$version-win32-amd64"
i686_name="boxrig-$version-win32-i686"

# Existing ZIP entries must not survive a new package build.
mkdir -p "$packages" "$root/$amd64_name"
printf 'obsolete documentation\n' > "$root/$amd64_name/README_ja.md"
cd "$root"
zip -q "$packages/$amd64_name.zip" "$amd64_name/README_ja.md"
"$repository/build_package.sh" --target native > "$root/build.log" 2>&1 || {
    cat "$root/build.log"
    exit 1
}
"$repository/build_package.sh" --target win32 >> "$root/build.log" 2>&1 || {
    cat "$root/build.log"
    exit 1
}
rm -rf "$root/$amd64_name"

tar -xzf "$packages/$linux_name.tar.gz" -C "$root"
unzip -q "$packages/$amd64_name.zip" -d "$root"
unzip -q "$packages/$i686_name.zip" -d "$root"
for name in "$linux_name" "$amd64_name" "$i686_name"; do
    sdk="$root/$name"
    cmp "$repository/README_pack.md" "$sdk/README.md"
    cmp "$repository/LICENSE" "$sdk/LICENSE"
    cmp "$repository/packaging/licenses/README.md" "$sdk/share/licenses/boxrig/README.md"
    cmp "$repository/deps/utf8proc/LICENSE.md" "$sdk/share/licenses/boxrig/utf8proc-LICENSE.md"
    cmp "$repository/deps/libpng/LICENSE" "$sdk/share/licenses/boxrig/libpng-LICENSE"
    cmp "$repository/deps/zlib/LICENSE" "$sdk/share/licenses/boxrig/zlib-LICENSE"
    if [ -e "$sdk/share/boxrig/examples" ]; then
        echo "FAIL example sources in $name" >&2
        exit 1
    fi
    if find "$sdk" \( -name '*_ja.md' -o -name '*.ja.md' -o -name ja \) -print | rg .; then
        echo "FAIL Japanese documentation in $name" >&2
        exit 1
    fi
done

sdk="$root/$linux_name"
for frontend in x11 gtk3 gtk4 wayland; do
    test -x "$sdk/bin/boxrig_term_$frontend"
done
test -x "$sdk/bin/boxrig_emitter"
test -s "$sdk/share/licenses/boxrig/Wayland-protocols-NOTICES.md"
cp "$repository/tests/win32/test_core_package.c" consumer.c
"$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror consumer.c \
    -I"$sdk/include" -L"$sdk/lib" -lboxrig -lm -o "$sdk/bin/consumer" \
    '-Wl,-rpath,$ORIGIN/../lib'
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/consumer"
export PKG_CONFIG_PATH="$sdk/lib/pkgconfig"
test "$(pkg-config --modversion boxrig)" = "$version"
"$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror \
    "$repository/tests/shared/consumer.c" \
    $(pkg-config --cflags --libs boxrig-ui boxrig-posix) -ldl \
    '-Wl,-rpath,$ORIGIN/../lib' -o "$sdk/bin/shared-consumer"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/shared-consumer"
for driver in selection_history selection_layout_provider; do
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/test_$driver.c" $(pkg-config --cflags --libs boxrig-ui) \
        '-Wl,-rpath,$ORIGIN/../lib' -o "$sdk/bin/$driver"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/$driver"
done
for specification in \
    'boxrig-gtk3:gtk3/test_gtk3_package.c' \
    'boxrig-gtk4:gtk4/test_gtk4_package.c' \
    'boxrig-wayland:wayland/test_wayland_package.c' \
    'boxrig-x11-ogl:x11/test_x11_opengl_package.c' \
    'boxrig-wayland-ogl:wayland/test_wayland_opengl_package.c'; do
    package=${specification%%:*}
    source=${specification#*:}
    # The other frontends remain static and need their private dependencies.
    mode=()
    if [[ "$package" != boxrig-gtk* ]]; then mode=(--static); fi
    "$compiler" -std=c99 -Wall -Wextra -Werror "$repository/tests/$source" \
        $(pkg-config --cflags --libs "${mode[@]}" "$package") \
        '-Wl,-rpath,$ORIGIN/../lib' -o "$sdk/bin/$package-consumer"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/$package-consumer"
done
PKG_CONFIG_PATH="$sdk/lib/pkgconfig" pkg-config --exists \
    boxrig-gtk3 boxrig-gtk4 boxrig-wayland boxrig-render-ogl \
    boxrig-x11-ogl boxrig-wayland-ogl boxrig-notification \
    boxrig-background-gtk3 boxrig-background-gtk4 boxrig-background-wayland

mkdir -p "$repository/build/tests/package-consumers"
unset PKG_CONFIG_PATH
for arch in amd64 i686; do
    sdk="$root/boxrig-$version-win32-$arch"
    compiler="$amd64_compiler"
    if [ "$arch" = i686 ]; then compiler="$i686_compiler"; fi
    export PKG_CONFIG_LIBDIR="$sdk/lib/pkgconfig"
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror consumer.c \
        $(pkg-config --cflags --libs boxrig) \
        -o "$repository/build/tests/package-consumers/$arch.exe"
    for driver in selection_history selection_layout_provider; do
        "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror \
            "$repository/tests/test_$driver.c" $(pkg-config --cflags --libs boxrig-win32) \
            -o "$repository/build/tests/package-consumers/$arch-$driver.exe"
    done
    pkg-config --exists boxrig-win32 boxrig-render-ogl boxrig-win32-ogl \
        boxrig-render-d3d11 boxrig-win32-d3d11
    test -s "$sdk/bin/boxrig_term.exe"
    test -s "$sdk/bin/boxrig_term_launcher.exe"
    test -s "$sdk/bin/boxrig_emitter.exe"
    test -s "$sdk/bin/boxrig-render-ogl.dll"
    test -s "$sdk/bin/boxrig-win32-ogl.dll"
    test -s "$sdk/share/licenses/boxrig/MinGW-w64-LICENSE.txt"
done
printf '%s\n' 'PASS packaged SDKs (Linux, Windows amd64/i686)'
